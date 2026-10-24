/* Tests for the *request* side of the toolbox actor: the two builders that turn
 * "the LLM answered with tool_calls" (`edc_toolbox_actor_dispatch`) and "this
 * task's ledger still holds PENDING records" (`edc_toolbox_actor_redispatch`)
 * into one `BC_ACTOR_MSG_TOOL_REQUEST` chain, plus the port wiring that puts
 * them on the scheduler (`edc_toolbox_actor_bind_scheduler`).
 *
 * Why a test of its own: every existing caller stubs this seam.
 * test_task_resume.c installs a stub on `sched->redispatch_tools` and
 * test_edc_scheduler.c stubs `dispatch_tools`, so the real implementations never
 * ran anywhere. What can break here is not "a message was sent" but:
 *
 *   - the chain carries exactly the records the ledger calls unfinished:
 *     PENDING in, EXECUTING out. The scheduler already synthesized a response
 *     for the EXECUTING ones on resume, so re-sending one runs the tool twice;
 *   - the chain is a deep copy -- `edc_toolbox_actor_req_free` must not free the
 *     ledger's sds/cJSON (that would leave the ledger pointing at freed memory
 *     and the next resume would read garbage);
 *   - an empty chain is not sent at all (and the dummy head never reaches the
 *     wire as an empty first request);
 *   - reply address / request_id / channel_type / task survive, so the response
 *     can be matched back to the ledger record it belongs to.
 *
 * Harness: the toolbox actor is added to a runner, but the loop is pumped by
 * this thread (`uv_run(..., UV_RUN_NOWAIT)`) instead of by a runner thread, so
 * the handler runs inline and the assertions need no locking. The actor's
 * handler is swapped for a capture -- the response side (tool execution) is a
 * different function and is not what this file is about. */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_exec_io.h"
#include "edc_scheduler.h"
#include "edc_task.h"
#include "edc_toolbox.h"
#include "edc_toolbox_actor.h"
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
 *  Capture side: what the toolbox actor actually receives
 *  ══════════════════════════════════════════════════════════════ */

#define MAX_CHAIN 8
#define STR_CAP 64

struct captured_req
{
    const void *name_ptr; /* for the aliasing probes: must differ from the ledger's */
    const void *args_ptr;
    const void *call_id_ptr;
    char name[STR_CAP];
    char call_id[STR_CAP];
    char args[STR_CAP]; /* unformatted JSON, "" when NULL */
    edc_channel_type call_from_type;
    struct edc_task *task;
};

static struct capture
{
    int msgs; /* messages that reached the actor */
    edc_actor_msg_type_t type;
    edc_actor_t *from;
    void *request_id;
    int chain_len; /* nodes walked (0 = nothing delivered) */
    struct captured_req req[MAX_CHAIN];
} cap;

static void capture_reset(void)
{
    memset(&cap, 0, sizeof(cap));
}

/* Owns the chain it is handed: detaches msg->data, records it, then frees it
 * node by node (the walk is itself an assertion target -- it proves the list the
 * builder linked is NULL-terminated and exactly as long as its return value
 * claimed). The single-node data_free_fn would only release the head. */
static void capture_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    (void)actor;
    cap.msgs++;
    cap.type = msg->type;
    cap.from = msg->from;
    cap.request_id = msg->request_id;

    struct edc_toolbox_actor_req *req = NULL;
    if (msg->type == BC_ACTOR_MSG_TOOL_REQUEST)
    {
        req = msg->data;
        msg->data = NULL;
    }

    int i = 0;
    for (; req && i < MAX_CHAIN; req = req->next, i++)
    {
        struct captured_req *c = &cap.req[i];
        c->name_ptr = req->name;
        c->args_ptr = req->args;
        c->call_id_ptr = req->call_id;
        snprintf(c->name, sizeof(c->name), "%s", req->name ? req->name : "(null)");
        snprintf(c->call_id, sizeof(c->call_id), "%s", req->call_id ? req->call_id : "(null)");
        if (req->args)
        {
            char *json = cJSON_PrintUnformatted(req->args);
            snprintf(c->args, sizeof(c->args), "%s", json ? json : "");
            cJSON_free(json);
        }
        else
        {
            c->args[0] = '\0';
        }
        c->call_from_type = req->call_from_type;
        c->task = req->task;
    }
    cap.chain_len = i;

    while (req)
    {
        struct edc_toolbox_actor_req *next = req->next;
        edc_toolbox_actor_req_free(req);
        req = next;
    }
    edc_actor_msg_free(msg);
}

/* ══════════════════════════════════════════════════════════════
 *  Reply port: what the thought echo carried (dispatch only)
 *  ══════════════════════════════════════════════════════════════ */

static int g_reply_calls = 0;
static sds g_reply_text = NULL;
static edc_channel_message_type g_reply_type = 0;
static void *g_reply_request_id = NULL;

static void record_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                              edc_channel_message_type message_type, void *request_id)
{
    (void)sched;
    (void)task;
    g_reply_calls++;
    if (g_reply_text)
        sdsfree(g_reply_text);
    g_reply_text = sdsnew(content ? content : "");
    g_reply_type = message_type;
    g_reply_request_id = request_id;
}

static void reply_capture_reset(void)
{
    g_reply_calls = 0;
    if (g_reply_text)
    {
        sdsfree(g_reply_text);
        g_reply_text = NULL;
    }
    g_reply_type = 0;
    g_reply_request_id = NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  Harness
 *  ══════════════════════════════════════════════════════════════ */

struct harness
{
    struct edc_config *cfg;
    struct edc_scheduler *sched;
    edc_actor_runner_t *runner;
    edc_actor_t *tb_actor;
};

static void close_handle_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, NULL);
}

static void harness_up(struct harness *h)
{
    h->cfg = g_new0(struct edc_config, 1);
    h->cfg->workspace = sdsnew(edc_test_workspace());
    h->cfg->tool_max_output_size = 100000;
    h->sched = edc_scheduler_new(h->cfg);
    h->sched->send_reply = record_send_reply;

    /* No toolbox instance: the response side is what reads state->toolbox, and
     * the capture handler never touches it. */
    h->tb_actor = edc_toolbox_actor_new(NULL);
    h->tb_actor->handler = capture_handler;

    h->runner = edc_actor_runner_new();
    edc_actor_runner_add(h->runner, h->tb_actor); /* actor->loop must exist or send() drops */
    reply_capture_reset();
}

/* The runner thread is never started: a few NOWAIT turns drain the wakeup and
 * run the handler on this thread. */
static void pump(struct harness *h)
{
    for (int i = 0; i < 4; i++)
        uv_run(&h->runner->loop, UV_RUN_NOWAIT);
}

static void harness_down(struct harness *h)
{
    /* Same teardown edc_actor_runner_stop does, for a loop we drove ourselves:
     * close the wakeup handle, drain the close callbacks, then the loop closes. */
    uv_walk(&h->runner->loop, close_handle_cb, NULL);
    uv_run(&h->runner->loop, UV_RUN_DEFAULT);
    edc_toolbox_actor_free(h->tb_actor); /* also clears the bind() registration */
    edc_actor_runner_free(h->runner);
    edc_scheduler_free(h->sched);
    edc_config_free(h->cfg);
    reply_capture_reset();
}

/* ── fixtures ─────────────────────────────────────────────── */

static cJSON *args_of(int n)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "n", n);
    return args;
}

/* `edc_task_tool_call_add` deep-copies args (the ledger owns its own), so the
 * object built here stays ours to free. */
static void ledger_add(struct edc_task *task, const char *call_id, const char *name, int argn)
{
    cJSON *args = argn ? args_of(argn) : NULL;
    edc_task_tool_call_add(task, call_id, name, args);
    if (args)
        cJSON_Delete(args);
}

static struct edc_task *new_task(const char *id)
{
    return edc_task_new(edc_test_id(id), "sess-tr", edc_test_id(id), "toolbox request test", edc_channel_adapter);
}

/* Same element description the llm actor builds its array with (a POD entry:
 * nothing to init/copy/free inside). */
static UT_icd tool_call_icd = {sizeof(struct edc_llm_chat_tool_call), NULL, NULL, NULL};

/* A response as the llm actor hands it over: one FUNCTION tool call. Ownership
 * of id/name/arguments stays with the caller until dispatch moves it. */
static void resp_add_call(struct edc_llm_actor_resp *resp, const char *id, const char *name, int argn)
{
    struct edc_llm_chat_tool_call tc = {0};
    tc.id = sdsnew(id);
    tc.type = BC_LLM_TOOL_CALL_TYPE_FUNCTION;
    tc.index = 0;
    tc.function.name = sdsnew(name);
    tc.function.arguments = args_of(argn);
    utarray_push_back(resp->tool_calls, &tc);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 1: the ledger's PENDING record -> one delivered request
 *  ══════════════════════════════════════════════════════════════ */

static void test_redispatch_pending_record(void)
{
    TEST("redispatch: a PENDING record becomes one delivered request");
    struct harness h;
    harness_up(&h);
    capture_reset();

    struct edc_task *t = new_task("t1");
    ledger_add(t, "call-a", "shell", 1);
    struct edc_tool_call_record *rec = edc_task_tool_call_find(t, "call-a");

    void *rid = (void *)0x4242;
    int n = edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, rid);
    CHECK(n == 1, "reports one rebuilt request");
    pump(&h);

    CHECK(cap.msgs == 1, "exactly one message reached the actor");
    CHECK(cap.type == BC_ACTOR_MSG_TOOL_REQUEST, "and it is a TOOL_REQUEST");
    CHECK(cap.from == edc_scheduler_get_actor(h.sched), "reply address is the scheduler actor");
    CHECK(cap.request_id == rid, "request_id survives (the response is matched by it)");
    CHECK(cap.chain_len == 1, "one node on the wire -- the dummy head is not sent");
    CHECK(strcmp(cap.req[0].name, "shell") == 0, "tool name carried");
    CHECK(strcmp(cap.req[0].call_id, "call-a") == 0, "call_id carried (matches the tool response)");
    CHECK(strcmp(cap.req[0].args, "{\"n\":1}") == 0, "args carried");
    CHECK(cap.req[0].call_from_type == edc_channel_adapter, "channel_type carried from the task");
    CHECK(cap.req[0].task == t, "task carried (tools read ctx->task)");

    /* The depth that matters: the request must not alias the ledger. */
    CHECK(cap.req[0].name_ptr != (const void *)rec->name, "name is a copy, not the ledger's sds");
    CHECK(cap.req[0].call_id_ptr != (const void *)rec->call_id, "call_id is a copy");
    CHECK(cap.req[0].args_ptr != (const void *)rec->args, "args is a copy, not the ledger's cJSON");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 2: EXECUTING is not re-dispatched, order is kept
 *  ══════════════════════════════════════════════════════════════ */

static void test_redispatch_skips_executing(void)
{
    TEST("redispatch: EXECUTING records are left out, ledger order kept");
    struct harness h;
    harness_up(&h);
    capture_reset();

    struct edc_task *t = new_task("t2");
    ledger_add(t, "c1", "shell", 1);
    ledger_add(t, "c2", "read_file", 2);
    ledger_add(t, "c3", "todo", 3);
    edc_task_tool_call_mark_executing(t, "c2");

    int n = edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, (void *)0x51);
    CHECK(n == 2, "two PENDING records rebuilt");
    pump(&h);

    CHECK(cap.chain_len == 2, "two nodes on the wire");
    CHECK(strcmp(cap.req[0].name, "shell") == 0 && strcmp(cap.req[1].name, "todo") == 0,
          "PENDING records in ledger order");
    CHECK(strcmp(cap.req[0].name, "read_file") != 0 && strcmp(cap.req[1].name, "read_file") != 0,
          "the EXECUTING tool is not re-run (the scheduler already answered it)");
    struct edc_tool_call_record *rec = edc_task_tool_call_find(t, "c2");
    CHECK(rec && rec->status == TOOL_CALL_EXECUTING, "and its record stays in the ledger untouched");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 3: nothing to redispatch -> nothing sent
 *  ══════════════════════════════════════════════════════════════ */

static void test_redispatch_nothing_to_send(void)
{
    TEST("redispatch: empty ledger / all EXECUTING -> 0 and no message");
    struct harness h;
    harness_up(&h);
    capture_reset();

    struct edc_task *t = new_task("t3");
    CHECK(edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, (void *)0x61) == 0, "empty ledger -> 0");
    pump(&h);
    CHECK(cap.msgs == 0, "nothing sent for an empty chain (no dummy head on the wire)");

    ledger_add(t, "c1", "shell", 1);
    edc_task_tool_call_mark_executing(t, "c1");
    CHECK(edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, (void *)0x62) == 0, "all EXECUTING -> 0");
    pump(&h);
    CHECK(cap.msgs == 0, "and still nothing sent");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 4: a record with nothing in it (defensive, but it is on disk)
 *  ══════════════════════════════════════════════════════════════ */

static void test_redispatch_empty_record(void)
{
    TEST("redispatch: a record with NULL name/args/call_id does not crash");
    struct harness h;
    harness_up(&h);
    capture_reset();

    struct edc_task *t = new_task("t4");
    edc_task_tool_call_add(t, NULL, NULL, NULL);

    int n = edc_toolbox_actor_redispatch(h.tb_actor, h.sched, t, (void *)0x71);
    CHECK(n == 1, "the record is still rebuilt");
    pump(&h);
    CHECK(cap.chain_len == 1, "one node on the wire");
    CHECK(cap.req[0].name_ptr != NULL && cap.req[0].name[0] == '\0', "missing name -> empty string, never NULL");
    CHECK(cap.req[0].args_ptr == NULL, "missing args stay NULL (nothing to run)");
    CHECK(cap.req[0].call_id_ptr == NULL, "missing call_id stays NULL");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 5: NULL arguments -> a no-op, not a crash
 *  ══════════════════════════════════════════════════════════════ */

static void test_redispatch_null_args(void)
{
    TEST("redispatch: NULL actor / NULL task -> 0, nothing sent");
    struct harness h;
    harness_up(&h);
    capture_reset();

    struct edc_task *t = new_task("t5");
    ledger_add(t, "c1", "shell", 1);

    CHECK(edc_toolbox_actor_redispatch(NULL, h.sched, t, (void *)0x81) == 0, "NULL actor -> 0");
    CHECK(edc_toolbox_actor_redispatch(h.tb_actor, h.sched, NULL, (void *)0x82) == 0, "NULL task -> 0");
    pump(&h);
    CHECK(cap.msgs == 0, "and nothing reached the actor");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 6: the port the resume path actually calls
 *  ══════════════════════════════════════════════════════════════ */

static void test_bind_scheduler_ports(void)
{
    TEST("bind: the scheduler's ports reach the real builders");
    struct harness h;
    harness_up(&h);
    capture_reset();

    edc_toolbox_actor_bind_scheduler(h.tb_actor, h.sched);
    CHECK(h.sched->redispatch_tools != NULL, "bind installs redispatch_tools");
    CHECK(h.sched->dispatch_tools != NULL, "bind installs dispatch_tools");

    struct edc_task *t = new_task("t6");
    ledger_add(t, "c1", "shell", 1);

    void *rid = (void *)0x91;
    int n = h.sched->redispatch_tools(h.sched, t, rid);
    CHECK(n == 1, "the port reaches the real builder");
    pump(&h);
    CHECK(cap.chain_len == 1 && strcmp(cap.req[0].name, "shell") == 0, "and delivers the same chain");
    CHECK(cap.request_id == rid, "request_id reaches the wire through the port");

    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 7: dispatch -- the LLM response builder
 *  ══════════════════════════════════════════════════════════════ */

static void test_dispatch_moves_ownership(void)
{
    TEST("dispatch: tool_calls -> one chain, ownership moved not copied");
    struct harness h;
    harness_up(&h);
    capture_reset();

    struct edc_task *t = new_task("t7");

    struct edc_llm_actor_resp *resp = g_new0(struct edc_llm_actor_resp, 1);
    resp->role = BC_LLM_CHAT_ROLE_ASSISTANT;
    resp->content = sdsnew("let me look");
    utarray_new(resp->tool_calls, &tool_call_icd);
    resp_add_call(resp, "x1", "shell", 7);
    resp_add_call(resp, "x2", "todo", 8);

    /* The pointers the response holds, to prove dispatch *moved* them. */
    struct edc_llm_chat_tool_call *tc0 = (struct edc_llm_chat_tool_call *)utarray_eltptr(resp->tool_calls, 0);
    sds name0 = tc0->function.name;
    cJSON *args0 = tc0->function.arguments;
    sds id0 = tc0->id;

    void *rid = (void *)0xa1;
    edc_toolbox_actor_dispatch(h.tb_actor, h.sched, t, resp, rid);
    pump(&h);

    CHECK(cap.msgs == 1 && cap.type == BC_ACTOR_MSG_TOOL_REQUEST, "one TOOL_REQUEST sent");
    CHECK(cap.chain_len == 2, "both FUNCTION calls are on one chain");
    CHECK(strcmp(cap.req[0].name, "shell") == 0 && strcmp(cap.req[1].name, "todo") == 0, "in response order");
    CHECK(strcmp(cap.req[1].args, "{\"n\":8}") == 0, "args carried");
    CHECK(cap.req[0].name_ptr == (const void *)name0, "the name is the same object (moved)");
    CHECK(cap.req[0].args_ptr == (const void *)args0, "the args are the same object (moved)");
    CHECK(cap.req[0].call_id_ptr == (const void *)id0, "the id is the same object (moved)");
    CHECK(cap.req[0].task == t, "task carried");
    CHECK(tc0->function.name == NULL && tc0->function.arguments == NULL && tc0->id == NULL,
          "the response no longer owns them (no double free)");
    CHECK(cap.request_id == rid, "request_id carried");

    CHECK(g_reply_calls == 1, "the thought content is echoed once");
    CHECK(g_reply_text && strcmp(g_reply_text, "let me look") == 0, "with the response content");
    CHECK(g_reply_type == EDC_CHANNEL_MESSAGE_THOUGHT_TYPE, "as a THOUGHT message");
    CHECK(g_reply_request_id == rid, "and under the same request_id");

    utarray_free(resp->tool_calls);
    sdsfree(resp->content);
    free(resp);
    edc_task_free(t);
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 8: dispatch without tool_calls / without content
 *  ══════════════════════════════════════════════════════════════ */

static void test_dispatch_no_tool_calls(void)
{
    TEST("dispatch: no tool_calls -> no chain; content falls back to reasoning");
    struct harness h;
    harness_up(&h);
    capture_reset();

    struct edc_task *t = new_task("t8");

    struct edc_llm_actor_resp *resp = g_new0(struct edc_llm_actor_resp, 1);
    resp->role = BC_LLM_CHAT_ROLE_ASSISTANT;
    edc_toolbox_actor_dispatch(h.tb_actor, h.sched, t, resp, (void *)0xb1);
    pump(&h);
    CHECK(cap.msgs == 0, "a response without tool_calls sends no chain");
    CHECK(g_reply_calls == 0, "and an empty response is not echoed");

    resp->reasoning_content = sdsnew("thinking out loud");
    g_reply_calls = 0;
    edc_toolbox_actor_dispatch(h.tb_actor, h.sched, t, resp, (void *)0xb2);
    pump(&h);
    CHECK(cap.msgs == 0, "still no chain");
    CHECK(g_reply_calls == 1 && g_reply_text && strcmp(g_reply_text, "thinking out loud") == 0,
          "reasoning-only content is echoed instead");

    CHECK(edc_toolbox_actor_redispatch(h.tb_actor, h.sched, NULL, (void *)0xb3) == 0, "still no-op on NULL task");
    edc_toolbox_actor_dispatch(NULL, h.sched, t, resp, (void *)0xb4);
    edc_toolbox_actor_dispatch(h.tb_actor, h.sched, t, NULL, (void *)0xb5);
    pump(&h);
    CHECK(cap.msgs == 0, "NULL actor / NULL response are no-ops");

    sdsfree(resp->reasoning_content);
    free(resp);
    edc_task_free(t);
    harness_down(&h);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== toolbox actor request side tests ===");
    log_info("");

    test_redispatch_pending_record();
    test_redispatch_skips_executing();
    test_redispatch_nothing_to_send();
    test_redispatch_empty_record();
    test_redispatch_null_args();
    test_bind_scheduler_ports();
    test_dispatch_moves_ownership();
    test_dispatch_no_tool_calls();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
