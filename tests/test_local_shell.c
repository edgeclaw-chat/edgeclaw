/* tests/test_local_shell.c -- `!cmd`: a user-typed shell command as a tool-only task.
 *
 * Pinned here:
 *   1. creation seeds the task right: tool_only + one PENDING call in the ledger + the
 *      matching assistant(tool_calls) message in the turn (a `role=tool` message must
 *      never be an orphan -- see edc_llm_request.c);
 *   2. driving it dispatches that call and settles on its result WITHOUT ever asking the
 *      LLM, and the turn (user + assistant + tool) reaches the group's history, so the
 *      next round sees the command and its output;
 *   3. the resume path where the call's result was lost: the task ends on the repaired
 *      response instead of falling through to an LLM request;
 *   4. the `!` entry itself: `!cmd` / `!!` / `\!` / bare `!`.
 *
 * See docs/local-shell-command.zh.md. */

#include "edc_local_shell.h"
#include "test_ws.h"

#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "log.h"

#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while (0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        log_error("    FAIL: " __VA_ARGS__); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while (0)

/* ── Minimal session/config/resolver scaffolding (same shape as
 *    test_edc_scheduler.c: the scheduler reads its session through `sessions`). ── */

static struct edc_config *make_config(void)
{
    struct edc_config *c = calloc(1, sizeof(struct edc_config));
    c->workspace = sdsnew(edc_test_workspace());
    return c;
}

static struct edc_session *make_session(struct edc_config *config)
{
    struct edc_session *s = calloc(1, sizeof(struct edc_session));
    s->session_id = edc_test_session_id("local-shell");
    s->config = config;
    return s;
}

static struct edc_session_manager *make_manager(struct edc_session *sess)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    return sm;
}

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    sched->sessions = make_manager(sess); /* the test process never frees it */
}

static int count_tasks(struct edc_session *s)
{
    int n = 0;
    for (struct edc_task_group *g = s->groups; g; g = g->next)
        n += g->member_count;
    return n;
}

/* How many tool-only tasks carry |cmd| as their seeded command (the members list is
 * appended to, so walk it through `group_next`). */
static int count_tool_only_with_cmd(struct edc_session *s, const char *cmd)
{
    int n = 0;
    for (struct edc_task_group *g = s->groups; g; g = g->next)
        for (struct edc_task *t = g->members; t; t = t->group_next)
            if (t->tool_only && t->tool_calls && t->tool_calls->args &&
                strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(t->tool_calls->args, "command")), cmd) == 0)
                n++;
    return n;
}

/* ── Ports (fakes: the test drives the events itself) ──────── */

static int g_llm_calls = 0;
static int g_redispatches = 0;
static int g_replies = 0;
static int g_replies_with_content = 0;

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched; (void)task; (void)request_id;
    g_llm_calls++;
}

static int mock_redispatch(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched; (void)task; (void)request_id;
    g_redispatches++;
    return 1; /* "one request posted" */
}

static void mock_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                            edc_channel_message_type message_type, void *request_id)
{
    (void)sched; (void)task; (void)message_type; (void)request_id;
    g_replies++;
    if (content && *content)
        g_replies_with_content++;
}

static void reset_counters(void)
{
    g_llm_calls = 0;
    g_redispatches = 0;
    g_replies = 0;
    g_replies_with_content = 0;
}

/* The message-end scan: every inbound message ends with it, and that is what drives a
 * READY task. A snapshot pull is the cheapest read-only message that reaches it. */
static void pump_message(struct edc_scheduler *sched)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    struct edc_session *owner = sched->ctx;
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, NULL, NULL,
                                             owner && owner->session_id ? sdsnew(edc_id_str(owner->session_id)) : NULL,
                                             (edc_actor_data_free_fn)sdsfree);
    actor->handler(actor, msg);
}

/* A single-tool chain TOOL_RESPONSE: the fake execution of the call. */
static void deliver_tool_resp(struct edc_scheduler *sched, long req_id, const char *call_id, const char *result)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    struct edc_toolbox_actor_req *tr = calloc(1, sizeof(*tr));
    tr->name = sdsnew("exec_shell");
    tr->call_id = sdsnew(call_id);
    tr->result = sdsnew(result);
    tr->next = NULL;
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TOOL_RESPONSE, NULL, (void *)(intptr_t)req_id, tr, NULL);
    actor->handler(actor, msg); /* dispatches + frees msg (data is ours) */
    sdsfree(tr->name);
    sdsfree(tr->call_id);
    sdsfree(tr->result);
    free(tr);
}

/* ── helpers ───────────────────────────────────────────────── */

static struct edc_task *create_tool_only(struct edc_scheduler *sched, struct edc_session *sess, const char *raw,
                                        const char *cmd)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", cmd);

    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.desc = cmd;
    spec.input = raw;
    spec.channel_type = edc_channel_adapter;
    spec.place = EDC_TASK_PLACE_FOREGROUND;
    spec.take_foreground = true;
    spec.initial_tool_name = "exec_shell";
    spec.initial_tool_args = args;

    struct edc_task *t = edc_scheduler_task_create(sched, sess, &spec);
    cJSON_Delete(args);
    return t;
}

static const char *msg_role(cJSON *m)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
}

static const char *msg_content(cJSON *m)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
}

/* ── 1. creation ───────────────────────────────────────────── */

static void test_creation_seeds_the_call(void)
{
    TEST("creation: tool_only + one PENDING call + the matching assistant(tool_calls)");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *t = create_tool_only(sched, sess, "!git status", "git status");
    CHECK(t != NULL, "task created");
    if (!t)
    {
        edc_scheduler_free(sched);
        free(sess);
        return;
    }

    CHECK(t->tool_only, "tool_only set (never reaches the LLM)");
    CHECK(t->sink == EDC_SINK_CHANNEL, "sink = CHANNEL: it IS conversation content");
    CHECK(edc_task_is_ready(t), "created READY -- driving is the scan's job");

    CHECK(edc_task_has_pending_tool_calls(t), "one call seeded in the ledger");
    struct edc_tool_call_record *rec = t->tool_calls;
    CHECK(rec && rec->name && strcmp(rec->name, "exec_shell") == 0, "the call is exec_shell");
    CHECK(rec && rec->status == TOOL_CALL_PENDING, "PENDING (nothing dispatched at creation)");
    CHECK(rec && rec->args &&
              strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(rec->args, "command")), "git status") == 0,
          "the args carry the command");
    CHECK(rec && rec->call_id && strncmp(rec->call_id, "call_local_", 11) == 0, "the call id names its owner");

    CHECK(t->active_turn != NULL, "a turn was attached");
    CHECK(t->active_turn && (int)t->active_turn->msg_count == 2,
          "turn = user + assistant(tool_calls)");
    cJSON *u = edc_turn_msg_at(t->active_turn, 0);
    cJSON *a = edc_turn_msg_at(t->active_turn, 1);
    CHECK(u && msg_role(u) && strcmp(msg_role(u), "user") == 0, "first message is the input");
    CHECK(u && msg_content(u) && strcmp(msg_content(u), "!git status") == 0, "…verbatim, as typed");
    CHECK(a && msg_role(a) && strcmp(msg_role(a), "assistant") == 0, "second is the assistant tool call");

    cJSON *tcs = a ? cJSON_GetObjectItem(a, "tool_calls") : NULL;
    CHECK(tcs && cJSON_IsArray(tcs) && cJSON_GetArraySize(tcs) == 1, "exactly one tool call");
    cJSON *fn = tcs ? cJSON_GetObjectItem(cJSON_GetArrayItem(tcs, 0), "function") : NULL;
    CHECK(fn && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name")), "exec_shell") == 0,
          "named exec_shell");
    CHECK(fn && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(fn, "arguments")), "{\"command\":\"git status\"}") == 0,
          "arguments is the JSON string the API expects (not an object)");

    edc_scheduler_free(sched);
    free(sess);
    edc_config_free(cfg);
}

/* ── 2. drive -> tool result -> settle, with no LLM ─────────── */

static void test_drive_dispatches_and_settles_without_llm(void)
{
    TEST("drive: the seeded call is dispatched, its result settles the task, the model is never asked");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *t = create_tool_only(sched, sess, "!git status", "git status");
    CHECK(t != NULL, "task created");

    reset_counters();
    sched->send_llm = mock_send_llm;
    sched->redispatch_tools = mock_redispatch;
    sched->send_reply = mock_send_reply;

    sds call_id = sdsdup(t->tool_calls->call_id);

    pump_message(sched);
    CHECK(g_redispatches == 1, "the seeded call was dispatched");
    CHECK(g_llm_calls == 0, "no LLM request was sent to start it");
    CHECK(t->step == EDC_TASKSTEP_AWAIT_TOOLS, "the task is waiting on its tool");

    long rid = (long)(intptr_t)t->current_request_id;
    deliver_tool_resp(sched, rid, call_id, "On branch main\nnothing to commit");
    sdsfree(call_id);

    CHECK(edc_task_is_completed(t), "completed on the tool result");
    CHECK(g_llm_calls == 0, "the model is never asked to continue");
    CHECK(t->active_turn == NULL, "the turn was committed");
    CHECK(!edc_task_has_pending_tool_calls(t), "the ledger was drained");
    CHECK(t->output && strcmp(t->output, "On branch main\nnothing to commit") == 0,
          "the task's output IS the command's output");

    CHECK(t->turns.tail == 1, "one committed turn");
    struct edc_turn *turn = t->turns.data[0];
    CHECK(turn && (int)turn->msg_count == 3, "user + assistant(tool_calls) + tool(result)");
    cJSON *tool_msg = turn ? edc_turn_msg_at(turn, 2) : NULL;
    CHECK(tool_msg && strcmp(msg_role(tool_msg), "tool") == 0, "the third message is the tool result");
    CHECK(tool_msg && strcmp(msg_content(tool_msg), "On branch main\nnothing to commit") == 0,
          "…carrying the output");

    /* "自动进入 group 历史": the very same messages are what the next request reads. */
    CHECK(t->group && edc_task_group_history_count(t->group) >= 3, "the turn landed in the group history");

    /* No RESP reply: the tool already echoed the output through its own TOOL_EMIT, and a
     * second copy would be rendered as Markdown. */
    CHECK(g_replies_with_content == 0, "nothing was delivered as the task's reply");

    edc_scheduler_free(sched);
    free(sess);
    edc_config_free(cfg);
}

/* ── 3. resume with the result lost ───────────────────────── */

static void test_lost_result_settles_instead_of_calling_llm(void)
{
    TEST("resume: a dispatched call whose result was lost ends the task (no LLM fallthrough)");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *t = create_tool_only(sched, sess, "!git status", "git status");
    CHECK(t != NULL, "task created");

    /* What a crash right after dispatch leaves behind: the call still in the ledger as
     * EXECUTING, no result. */
    edc_task_tool_call_mark_executing(t, t->tool_calls->call_id);

    reset_counters();
    sched->send_llm = mock_send_llm;
    sched->redispatch_tools = mock_redispatch;
    sched->send_reply = mock_send_reply;

    pump_message(sched);

    CHECK(edc_task_is_completed(t), "the task settles");
    CHECK(g_llm_calls == 0, "the repair must NOT fall through to an LLM request");
    CHECK(g_redispatches == 0, "nothing is re-dispatched (the call already ran)");
    CHECK(!edc_task_has_pending_tool_calls(t), "the ledger was drained by the repair");
    CHECK(t->output && strstr(t->output, "interrupted") != NULL, "the output says the result was lost");
    CHECK(t->turns.tail == 1 &&
              ((struct edc_turn *)t->turns.data[0])->msg_count == 3,
          "turn = user + assistant(tool_calls) + the repaired tool response");

    edc_scheduler_free(sched);
    free(sess);
    edc_config_free(cfg);
}

/* ── 4. the `!` entry ──────────────────────────────────────── */

static void test_bang_entry(void)
{
    TEST("`!` entry: `!cmd` creates the task, `!!` re-runs it, `\\!` and other input are declined");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    struct edc_session_manager *sm = sched->sessions;

    edc_actor_t *ch = edc_channel_actor_new();
    struct edc_reply_to to = {.channel_actor = ch, .actor = ch, .type = EDC_CHANNEL_MESSAGE_SYS_TYPE};

    /* `!cmd` */
    struct edc_channel_message *m =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew("!  git status"));
    CHECK(edc_local_shell_handle(sm, sched, to, m), "`!` input is taken");
    edc_channel_message_free(m);
    CHECK(count_tasks(sess) == 1, "one task was created");
    CHECK(sess->last_local_cmd && strcmp(sess->last_local_cmd, "git status") == 0,
          "the command is remembered for `!!` (leading blanks dropped)");
    struct edc_task *first = sched ? edc_scheduler_get_foreground(sess)->leader : NULL;
    CHECK(first && first->tool_only, "it is a tool-only task");

    /* `!!` re-runs the previous command (a second task, same command). */
    m = edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew("!!"));
    CHECK(edc_local_shell_handle(sm, sched, to, m), "`!!` is taken");
    edc_channel_message_free(m);
    CHECK(count_tasks(sess) == 2, "`!!` created a second task");
    CHECK(count_tool_only_with_cmd(sess, "git status") == 2, "`!!` re-ran the same command");

    /* Bare `!`: answered, nothing created. */
    m = edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew("!"));
    CHECK(edc_local_shell_handle(sm, sched, to, m), "bare `!` is taken (it answers with usage)");
    edc_channel_message_free(m);
    CHECK(count_tasks(sess) == 2, "bare `!` created no task");

    /* Not ours: `\!` (the escape) and any ordinary input. */
    m = edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew("\\!git status"));
    CHECK(!edc_local_shell_handle(sm, sched, to, m), "`\\!` is handed back to the ordinary input path");
    edc_channel_message_free(m);

    m = edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew("hello"));
    CHECK(!edc_local_shell_handle(sm, sched, to, m), "ordinary input is declined");
    edc_channel_message_free(m);

    edc_scheduler_free(sched);
    free(sess);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace(); /* disposable store for anything that writes state */
    log_info("=== `!cmd` (tool-only task) tests ===");
    log_info("");

    test_creation_seeds_the_call();
    test_drive_dispatches_and_settles_without_llm();
    test_lost_result_settles_instead_of_calling_llm();
    test_bang_entry();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
