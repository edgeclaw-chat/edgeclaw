/* Tests for the *response* side of the toolbox actor: what happens after a
 * TOOL_REQUEST chain reaches the actor.
 *
 *   tool executes -> BC_ACTOR_MSG_TOOL_STARTED -> scheduler actor
 *                 -> sched_handle_tool_started -> executor tool_started()
 *                 -> edc_task_tool_call_mark_executing()      (PENDING -> EXECUTING)
 *   result        -> BC_ACTOR_MSG_TOOL_RESPONSE -> scheduler actor
 *                 -> sched_handle_tool_resp -> executor tool_done()
 *                 -> the turn gets a tool message, the ledger record is removed,
 *                    and either the next node starts or the turn continues with
 *                    an LLM request.
 *
 * test_toolbox_request.c stops at "the request chain was delivered correctly";
 * this half crosses two actors' mailboxes, the real toolbox lookup, a real tool
 * implementation, the callback, the return message and the task state machine.
 * test_task_resume.c asserts "c1 stays PENDING until toolbox reports started",
 * but it installs a stub on redispatch_tools, so nothing ever did report
 * started: the evidence that the ledger flips to EXECUTING was missing, and that
 * flip is the only thing separating "re-run this tool on resume" from "this one
 * was already running".
 *
 * Harness: a real toolbox (defaults registered) plus a probe tool that *defers*
 * its callback, so the test can look at the world between "started" and "done"
 * -- which is exactly where the ledger has to say EXECUTING. Both actors sit on
 * a runner whose loop this thread pumps (UV_RUN_NOWAIT), so the handlers run
 * inline and the assertions need no locking.
 *
 * No session is installed: with sched->sessions == NULL the per-message scan and
 * the dirty drain are no-ops, which is what keeps this round trip deterministic
 * (driving task trees is the scan's subject, not this file's). */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_error.h"
#include "edc_exec_io.h"
#include "edc_scheduler.h"
#include "edc_task.h"
#include "edc_toolbox.h"
#include "edc_toolbox_actor.h"
#include "edc_tools.h"
#include "edc_turn.h"
#include "log.h"
#include <cJSON.h>
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

/* Stub for _run_loop pulled in by edc_commands (transitively linked). */
volatile sig_atomic_t _run_loop = 1;

/* ══════════════════════════════════════════════════════════════
 *  Test framework
 *  ══════════════════════════════════════════════════════════════ */

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        log_info("  TEST: %s", name);                                                                                  \
    } while (0)
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: %s", msg);                                                                            \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            test_passed++;                                                                                             \
        }                                                                                                              \
    } while (0)

/* ══════════════════════════════════════════════════════════════
 *  The probe tool: records what it was handed, then waits
 *  ══════════════════════════════════════════════════════════════ */

#define PROBE_MAX 4
#define STR_CAP 128

struct probe_call
{
    struct edc_toolbox_call_ctx *ctx;
    void (*cb)(struct edc_toolbox_call_ctx *, sds, EdcError *);
    char name[STR_CAP];
    char args[STR_CAP];
};

static struct probe
{
    int calls;
    edc_channel_type call_from_type;
    struct edc_task *task;
    void *request_id;
    struct probe_call call[PROBE_MAX];
} probe;

static void probe_reset(void)
{
    memset(&probe, 0, sizeof(probe));
}

static void probe_exec(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                       void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    int i = probe.calls++;
    if (i >= PROBE_MAX)
        return;

    struct probe_call *c = &probe.call[i];
    c->ctx = ctx;
    c->cb = callback;
    snprintf(c->name, sizeof(c->name), "%s", (tool && tool->name) ? tool->name : "");
    char *json = args ? cJSON_PrintUnformatted(args) : NULL;
    snprintf(c->args, sizeof(c->args), "%s", json ? json : "");
    if (json)
        cJSON_free(json);

    /* Routing info the tool sees: it must be the task's, or the result would be
     * attributed to the wrong place. */
    probe.call_from_type = ctx->call_from_type;
    probe.task = ctx->task;
    probe.request_id = ctx->request_id;
}

/* The tool "finishes". Deliberately called by the test, not by the tool. */
static void probe_finish(int i, const char *result)
{
    struct probe_call *c = &probe.call[i];
    void (*cb)(struct edc_toolbox_call_ctx *, sds, EdcError *) = c->cb;
    if (!cb)
        return; /* that call never started; the assertions around it say so */
    c->cb = NULL;
    cb(c->ctx, sdsnew(result), NULL);
}

/* Finish with a result sds AND an error, which is what edc_tools_memory does
 * (`callback(ctx, err, edc_error_new(...))` where `err` is the message it built). The
 * actor's callback keeps the error and must release the result -- overwriting the variable
 * leaked one sds per failed memory call. */
static void probe_finish_with_result_and_error(int i, const char *result, const char *error_message)
{
    struct probe_call *c = &probe.call[i];
    void (*cb)(struct edc_toolbox_call_ctx *, sds, EdcError *) = c->cb;
    if (!cb)
        return;
    c->cb = NULL;
    cb(c->ctx, sdsnew(result), edc_error_new(EDC_ERR_INVAL, "%s", error_message));
}

/* ══════════════════════════════════════════════════════════════
 *  Ports the scheduler drives
 *  ══════════════════════════════════════════════════════════════ */

static int g_send_llm_calls = 0;

static void record_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)task;
    (void)request_id;
    g_send_llm_calls++;
}

/* ══════════════════════════════════════════════════════════════
 *  Harness
 *  ══════════════════════════════════════════════════════════════ */

struct harness
{
    struct edc_config *cfg;
    struct edc_scheduler *sched;
    struct edc_toolbox *tb;
    edc_actor_t *tb_actor;
    edc_actor_runner_t *runner;
};

static void close_handle_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, NULL);
}

static void harness_up(struct harness *h, int max_output)
{
    h->cfg = g_new0(struct edc_config, 1);
    h->cfg->workspace = sdsnew(edc_test_workspace());
    h->cfg->tool_max_output_size = max_output;

    h->sched = edc_scheduler_new(h->cfg);
    h->sched->send_llm = record_send_llm; /* the chain tail asks for the next round */

    h->tb = edc_toolbox_new(h->cfg, edc_scheduler_get_actor(h->sched), NULL);
    edc_toolbox_register(h->tb, edc_tool_new("t_probe", "test probe", NULL, probe_exec));
    h->tb_actor = edc_toolbox_actor_new(h->tb);

    /* Both sides are real: the toolbox actor to run the tool, the scheduler actor
     * to receive STARTED/RESPONSE and advance the task. */
    h->runner = edc_actor_runner_new();
    edc_actor_runner_add(h->runner, edc_scheduler_get_actor(h->sched));
    edc_actor_runner_add(h->runner, h->tb_actor);

    probe_reset();
    g_send_llm_calls = 0;
}

/* The runner thread is never started: a few NOWAIT turns drain both mailboxes,
 * including the messages the handlers send each other while they run. */
static void pump(struct harness *h)
{
    for (int i = 0; i < 8; i++)
        uv_run(&h->runner->loop, UV_RUN_NOWAIT);
}

static void harness_down(struct harness *h)
{
    uv_walk(&h->runner->loop, close_handle_cb, NULL);
    uv_run(&h->runner->loop, UV_RUN_DEFAULT);
    edc_toolbox_actor_free(h->tb_actor); /* reclaims anything still in flight */
    edc_actor_runner_free(h->runner);
    edc_scheduler_free(h->sched);
    edc_toolbox_free(h->tb);
    edc_config_free(h->cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Fixtures
 *  ══════════════════════════════════════════════════════════════ */

static cJSON *args_of(int n)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "n", n);
    return args;
}

static void ledger_add(struct edc_task *task, const char *call_id, const char *name, int argn)
{
    cJSON *args = argn ? args_of(argn) : NULL;
    edc_task_tool_call_add(task, call_id, name, args);
    if (args)
        cJSON_Delete(args);
}

/* An active turn ending in an assistant message that asked for these calls --
 * the shape the tool results have to be appended to. */
static void seed_tool_turn(struct edc_task *t, const char *const *call_ids, int count)
{
    t->active_turn = edc_turn_new();
    edc_turn_add_user_message(t->active_turn, "please run the tools");

    cJSON *asst = cJSON_CreateObject();
    cJSON_AddStringToObject(asst, "role", "assistant");
    cJSON *tcs = cJSON_CreateArray();
    for (int i = 0; i < count; i++)
    {
        cJSON *tc = cJSON_CreateObject();
        cJSON_AddStringToObject(tc, "id", call_ids[i]);
        cJSON_AddStringToObject(tc, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", "t_probe");
        cJSON_AddStringToObject(fn, "arguments", "{\"n\":1}");
        cJSON_AddItemToObject(tc, "function", fn);
        cJSON_AddItemToArray(tcs, tc);
    }
    cJSON_AddItemToObject(asst, "tool_calls", tcs);
    edc_turn_add_assistant_message(t->active_turn, asst);
}

static const char *turn_tool_content(struct edc_turn *turn, const char *call_id)
{
    if (!turn)
        return NULL;
    for (int i = 0; i < turn->msg_count; i++)
    {
        cJSON *m = edc_turn_msg_at(turn, i);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcid = cJSON_GetObjectItem(m, "tool_call_id");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0 && tcid && cJSON_IsString(tcid) &&
            strcmp(tcid->valuestring, call_id) == 0)
            return cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
    }
    return NULL;
}

static int turn_count_tool_msgs(struct edc_turn *turn)
{
    int n = 0;
    if (!turn)
        return 0;
    for (int i = 0; i < turn->msg_count; i++)
    {
        cJSON *m = edc_turn_msg_at(turn, i);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0)
            n++;
    }
    return n;
}

/* A task parked in AWAIT_TOOLS with a live turn: "tool calls are out". */
static struct edc_task *new_waiting_task(const char *id, const char *const *call_ids, int count)
{
    struct edc_task *t = edc_task_new(edc_test_id(id), edc_test_session_id("sess-tb"), edc_test_id(id), "toolbox response test", edc_channel_adapter);
    edc_task_await_exec(t);
    edc_task_await_tools(t);
    seed_tool_turn(t, call_ids, count);
    return t;
}

/* ══════════════════════════════════════════════════════════════
 *  Case 1: the whole round trip, node by node
 *  ══════════════════════════════════════════════════════════════ */

static void test_round_trip_two_nodes(void)
{
    TEST("two PENDING tools run in series and the ledger tracks each one");
    struct harness h;
    harness_up(&h, 100000);

    const char *ids[] = {"c1", "c2"};
    struct edc_task *t = new_waiting_task("t1", ids, 2);
    ledger_add(t, "c1", "t_probe", 1);
    ledger_add(t, "c2", "t_probe", 2);

    void *rid = (void *)0x1001;
    edc_scheduler_register_task(h.sched, t, rid);
    edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, rid);
    pump(&h);

    /* ── the tool started, the result is not back yet ────────── */
    CHECK(probe.calls == 1, "only the first node started (the chain is serial)");
    CHECK(strcmp(probe.call[0].name, "t_probe") == 0, "the probe is the tool that ran");
    CHECK(strcmp(probe.call[0].args, "{\"n\":1}") == 0, "it got the first node's args");
    CHECK(probe.call_from_type == edc_channel_adapter, "the tool sees the task's channel type");
    CHECK(probe.task == t, "and the task that asked for it");
    CHECK(probe.request_id == rid, "and the request id, so its result can be routed back");

    struct edc_tool_call_record *r1 = edc_task_tool_call_find(t, "c1");
    struct edc_tool_call_record *r2 = edc_task_tool_call_find(t, "c2");
    CHECK(r1 && r1->status == TOOL_CALL_EXECUTING, "c1 flipped PENDING -> EXECUTING on the STARTED report");
    CHECK(r2 && r2->status == TOOL_CALL_PENDING, "c2 is untouched until its turn");
    CHECK(g_send_llm_calls == 0, "no LLM request while tools are outstanding");
    CHECK(turn_count_tool_msgs(t->active_turn) == 0, "and no result recorded yet");

    /* ── the first tool finishes ─────────────────────────────── */
    probe_finish(0, "result-one");
    pump(&h);

    CHECK(probe.calls == 2, "the second node starts only after the first answered");
    CHECK(strcmp(probe.call[1].args, "{\"n\":2}") == 0, "with its own args");
    CHECK(edc_task_tool_call_find(t, "c1") == NULL, "c1 left the ledger (its result arrived)");
    r2 = edc_task_tool_call_find(t, "c2");
    CHECK(r2 && r2->status == TOOL_CALL_EXECUTING, "c2 flipped to EXECUTING");
    CHECK(g_send_llm_calls == 0, "still no LLM request: c2 has not answered");
    const char *content = turn_tool_content(t->active_turn, "c1");
    CHECK(content && strcmp(content, "result-one") == 0, "c1's result landed in the turn");
    CHECK(turn_tool_content(t->active_turn, "c2") == NULL, "and c2 has none yet");

    /* ── the chain tail finishes ─────────────────────────────── */
    probe_finish(1, "result-two");
    pump(&h);

    CHECK(edc_task_tool_call_find(t, "c2") == NULL, "c2 left the ledger too");
    CHECK(turn_count_tool_msgs(t->active_turn) == 2, "both results are in the turn");
    CHECK(g_send_llm_calls == 1, "the finished chain continues the turn with one LLM request");
    CHECK(t->step == EDC_TASKSTEP_AWAIT_EXEC, "and the task is back to AWAIT_EXEC");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 1b: a tool that fails AND hands over a result sds
 *
 *  The memory tools do this: they build the error message as an sds, pass it as the
 *  result, and wrap the same text in an EdcError. The actor keeps the error, so it has to
 *  release the result -- it used to overwrite the variable and leak the string, once per
 *  failed memory call. Run under valgrind, this case is the difference.
 *  ══════════════════════════════════════════════════════════════ */

static void test_result_and_error_together(void)
{
    TEST("a call that returns BOTH a result and an error keeps the error and frees the result");
    struct harness h;
    harness_up(&h, 100000);

    const char *ids[] = {"c1"};
    struct edc_task *t = new_waiting_task("t1b", ids, 1);
    edc_task_tool_call_add(t, "c1", "t_probe", cJSON_Parse("{}"));

    void *rid = (void *)0x100b;
    edc_scheduler_register_task(h.sched, t, rid);
    edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, rid);
    pump(&h);
    CHECK(probe.calls == 1, "the tool was entered");

    probe_finish_with_result_and_error(0, "the message the tool built", "session memory is not reachable");
    pump(&h);

    const char *content = turn_tool_content(t->active_turn, "c1");
    CHECK(content != NULL, "the turn got a tool message");
    CHECK(content && strstr(content, "session memory is not reachable") != NULL,
          "the ERROR is what reaches the model, not the result it superseded");
    CHECK(content && strstr(content, "the message the tool built") == NULL,
          "and the superseded result is not duplicated into it");
    CHECK(content && strstr(content, "Analyze the error above") != NULL, "with the retry hint");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 2: a tool that cannot run still answers
 *  ══════════════════════════════════════════════════════════════ */

static void test_invalid_args_round_trip(void)
{
    TEST("a call with unusable arguments answers with the error instead of hanging");
    struct harness h;
    harness_up(&h, 100000);

    const char *ids[] = {"c1"};
    struct edc_task *t = new_waiting_task("t2", ids, 1);
    /* args missing: the ledger can hold this (it comes off disk), and the tool
     * must not be reached with it. */
    edc_task_tool_call_add(t, "c1", "t_probe", NULL);

    void *rid = (void *)0x1002;
    edc_scheduler_register_task(h.sched, t, rid);
    edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, rid);
    pump(&h);

    CHECK(probe.calls == 0, "the tool was never entered");
    CHECK(edc_task_tool_call_find(t, "c1") == NULL, "the ledger record is settled, not stuck");
    const char *content = turn_tool_content(t->active_turn, "c1");
    CHECK(content != NULL, "the turn got a tool message anyway");
    CHECK(content && strstr(content, "arguments must be a JSON object") != NULL, "carrying the validation error");
    CHECK(content && strstr(content, "Analyze the error above") != NULL, "plus the retry hint");
    CHECK(g_send_llm_calls == 1, "and the turn continues (the model gets to retry)");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 3: the output cap is applied on the way back
 *  ══════════════════════════════════════════════════════════════ */

static void test_result_truncated(void)
{
    TEST("a long result is capped before it enters the turn");
    struct harness h;
    harness_up(&h, 10); /* tool_max_output_size */

    const char *ids[] = {"c1"};
    struct edc_task *t = new_waiting_task("t3", ids, 1);
    ledger_add(t, "c1", "t_probe", 1);

    void *rid = (void *)0x1003;
    edc_scheduler_register_task(h.sched, t, rid);
    edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, rid);
    pump(&h);
    probe_finish(0, "0123456789ABCDEFGHIJ");
    pump(&h);

    const char *content = turn_tool_content(t->active_turn, "c1");
    CHECK(content != NULL, "the result still arrives");
    /* The notice is at the HEAD, deliberately: a preview is cut from the front, so a notice
     * appended at the tail is exactly the part that disappears when something else caps the
     * result again (which is what used to happen to the grep/find caps). */
    CHECK(content && strstr(content, "[truncated: showing") != NULL, "a truncation notice comes first");
    CHECK(content && strstr(content, "20 bytes") != NULL, "naming the original size");
    CHECK(content && strstr(content, "0123456789") != NULL, "the preview keeps the start");
    CHECK(content && strstr(content, "ABCDEFGHIJ") == NULL, "the tail is not in the message");
    /* And the whole thing is recoverable: either a path to read, or an explicit statement
     * that it could not be saved (never silence about where it went). */
    CHECK(content && (strstr(content, "[full output: ") != NULL || strstr(content, "could not be saved") != NULL),
          "the message says where the rest is");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 4: a result that arrives after the request was cancelled
 *  ══════════════════════════════════════════════════════════════ */

static void test_late_result_after_cancel(void)
{
    TEST("a late tool result is dropped once the request is cancelled");
    struct harness h;
    harness_up(&h, 100000);

    const char *ids[] = {"c1"};
    struct edc_task *t = new_waiting_task("t4", ids, 1);
    ledger_add(t, "c1", "t_probe", 1);

    void *rid = (void *)0x1004;
    edc_scheduler_register_task(h.sched, t, rid);
    edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, rid);
    pump(&h);
    CHECK(probe.calls == 1, "the tool is running");

    /* The cancel lands while the tool is out. The flag is set directly here so
     * the gate is tested on its own; the cancel path itself also clears the
     * ledger, which is a different subject. */
    t->cancelled_request = 1;
    probe_finish(0, "too late");
    pump(&h);

    CHECK(g_send_llm_calls == 0, "no LLM request from a cancelled request");
    CHECK(turn_count_tool_msgs(t->active_turn) == 0, "and the stale result is not written into the turn");
    struct edc_tool_call_record *r1 = edc_task_tool_call_find(t, "c1");
    CHECK(r1 != NULL, "the ledger record is left alone (the cancel path owns cleaning it up)");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 5: the STARTED report itself can be too late
 *  ══════════════════════════════════════════════════════════════ */

static void test_started_after_cancel(void)
{
    TEST("a STARTED report that arrives after the cancel does not mark the ledger");
    struct harness h;
    harness_up(&h, 100000);

    const char *ids[] = {"c1"};
    struct edc_task *t = new_waiting_task("t5", ids, 1);
    ledger_add(t, "c1", "t_probe", 1);

    void *rid = (void *)0x1005;
    edc_scheduler_register_task(h.sched, t, rid); /* clears the flag... */
    t->cancelled_request = 1;                     /* ...so cancel it again, as a real cancel would */

    edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, rid);
    pump(&h);

    /* The toolbox has no business knowing about cancels: it runs what it was
     * asked to run. Refusing the report is the scheduler's job. */
    CHECK(probe.calls == 1, "the tool runs anyway (the request was already delivered)");
    struct edc_tool_call_record *r1 = edc_task_tool_call_find(t, "c1");
    CHECK(r1 && r1->status == TOOL_CALL_PENDING, "but the ledger stays PENDING: no EXECUTING report was accepted");
    CHECK(g_send_llm_calls == 0, "and nothing continues");

    /* Left in flight on purpose: harness_down has to reclaim it (that is the
     * teardown path for a tool that never calls back). */
    edc_task_free(t);
    harness_down(&h);
}

int main(void)
{
    test_result_and_error_together();
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== toolbox actor response side tests ===");
    log_info("");

    test_round_trip_two_nodes();
    test_invalid_args_round_trip();
    test_result_truncated();
    test_late_result_after_cancel();
    test_started_after_cancel();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
