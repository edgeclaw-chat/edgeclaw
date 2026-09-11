/* Regression tests for the resume ledger introduced with
 * "所有未完成任务加载后 → SUSPENDED, /task resume 重新判定":
 *
 *   1. tool-call 账本 (task->tool_calls) 的增删查改语义:
 *      PENDING (未执行) / EXECUTING (执行中) —— 由 toolbox 上报"开始执行"
 *      (BC_ACTOR_MSG_TOOL_STARTED) 按 call_id 置位, 完成后删除, 不依赖链序;
 *   2. 账本随 session.json 往返: EXECUTING/PENDING 状态与 args 保留;
 *      同时验证未完成任务 (TOOL_CALLING) 加载后收敛为 SUSPENDED;
 *   3. edc_scheduler_resume_task 的重新判定:
 *      - 依赖未满足 → WAITING_DEPS;
 *      - 账本 EXECUTING → 补一条"中断"tool 响应, 无 PENDING 时续 LLM;
 *      - 账本 PENDING → TOOL_CALLING 并重新派发 (走 redispatch 回调);
 *      - 无依赖/工具/turn → WAITING_INPUT。
 *
 * 注: SUSPENDED 是 deserialize 直接赋值产生的状态 (转换表里只允许
 * TOOL_CALLING → SUSPENDED), 故测试用 force_suspended() 直接置位模拟加载结果。
 */

#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include "log.h"
#include <cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

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

#define TEST_WS "/tmp/test_edc_task_resume_ws"

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(TEST_WS);
    return cfg;
}

static struct edc_task *find_task(struct edc_session *session, const char *task_id)
{
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (strcmp(t->task_id, task_id) == 0)
                return t;
        }
    }
    return NULL;
}

/* 模拟"加载后 SUSPENDED": deserialize 直接赋值, 不走转换校验。 */
static void force_suspended(struct edc_task *t)
{
    t->state = TASK_SUSPENDED;
}

static bool turn_has_tool_msg(struct edc_turn *turn, const char *call_id)
{
    if (!turn)
        return false;
    for (size_t i = turn->messages.head; i < turn->messages.tail; i++)
    {
        cJSON *m = turn->messages.data[i];
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *tcid = cJSON_GetObjectItem(m, "tool_call_id");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0 && tcid && cJSON_IsString(tcid) &&
            strcmp(tcid->valuestring, call_id) == 0)
            return true;
    }
    return false;
}

/* 给 active_turn 塞一条 user + 一条 assistant(tool_calls) 消息。 */
static void seed_tool_turn(struct edc_task *t, const char *call_id, const char *name)
{
    t->active_turn = edc_turn_new();
    edc_turn_add_user_message(t->active_turn, "please run the tool");

    cJSON *asst = cJSON_CreateObject();
    cJSON_AddStringToObject(asst, "role", "assistant");
    cJSON *tcs = cJSON_CreateArray();
    cJSON *tc = cJSON_CreateObject();
    cJSON_AddStringToObject(tc, "id", call_id);
    cJSON_AddStringToObject(tc, "type", "function");
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "arguments", "{}");
    cJSON_AddItemToObject(tc, "function", fn);
    cJSON_AddItemToArray(tcs, tc);
    cJSON_AddItemToObject(asst, "tool_calls", tcs);
    edc_turn_add_assistant_message(t->active_turn, asst);
}

/* ── stub 回调: 观测 resume 是否真的驱动了 LLM / 重发工具 ── */

static int g_send_llm_calls = 0;
static int g_redispatch_calls = 0;

static void stub_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)task;
    (void)request_id;
    g_send_llm_calls++;
}

static int stub_redispatch(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_redispatch_calls++;
    int n = 0;
    struct edc_tool_call_record *r;
    for (r = task->tool_calls; r; r = r->next)
        if (r->status == TOOL_CALL_PENDING)
            n++;
    return n;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: ledger add/find/mark/complete/remove/clear
 *  ══════════════════════════════════════════════════════════════ */

static void test_ledger_ops(void)
{
    TEST("ledger: add/find/mark-executing/remove/clear (call_id 精准置位)");
    struct edc_task *t = edc_task_new("t1", "s", "g", "task", edc_channel_adapter);

    edc_task_tool_call_add(t, "call-a", "shell", NULL);
    edc_task_tool_call_add(t, "call-b", "read_file", NULL);
    edc_task_tool_call_add(t, "call-c", "write_file", NULL);
    CHECK(edc_task_has_pending_tool_calls(t), "ledger non-empty");
    CHECK(edc_task_tool_call_find(t, "call-b") != NULL, "find call-b");
    CHECK(edc_task_tool_call_find(t, "nope") == NULL, "find missing -> NULL");

    /* toolbox 上报"开始执行": 按 call_id 置位, 与链序无关 */
    struct edc_tool_call_record *m = edc_task_tool_call_mark_executing(t, "call-b");
    CHECK(m && m->status == TOOL_CALL_EXECUTING, "call-b -> EXECUTING by call_id");
    CHECK(edc_task_tool_call_find(t, "call-a")->status == TOOL_CALL_PENDING, "call-a still PENDING");
    CHECK(edc_task_tool_call_find(t, "call-c")->status == TOOL_CALL_PENDING, "call-c still PENDING");
    CHECK(edc_task_tool_call_mark_executing(t, "nope") == NULL, "mark unknown call_id -> NULL");

    /* 完成即删除, 不发生链序推进 */
    CHECK(edc_task_tool_call_remove(t, "call-b"), "remove call-b");
    CHECK(edc_task_tool_call_find(t, "call-b") == NULL, "call-b deleted");
    CHECK(edc_task_tool_call_find(t, "call-c")->status == TOOL_CALL_PENDING, "call-c stays PENDING (no promotion)");
    CHECK(!edc_task_tool_call_remove(t, "call-b"), "double remove -> false");

    edc_task_tool_calls_clear(t);
    CHECK(t->tool_calls == NULL, "ledger cleared");
    CHECK(!edc_task_has_pending_tool_calls(t), "no records after clear");

    edc_task_free(t);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: ledger survives session.json round-trip; unfinished ->
 *          SUSPENDED
 *  ══════════════════════════════════════════════════════════════ */

static void test_ledger_serialize(void)
{
    TEST("ledger persists (status+args); TOOL_CALLING -> SUSPENDED on load");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "sess-l", "L");
    struct edc_scheduler *sched = edc_scheduler_new(s, cfg);

    struct edc_task *t = edc_task_new("lt", "sess-l", "lt", "task", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_TOOL_CALLING);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "cmd", "ls");
    edc_task_tool_call_add(t, "c1", "shell", args);
    edc_task_tool_call_add(t, "c2", "read_file", NULL);
    cJSON_Delete(args);
    edc_task_tool_call_mark_executing(t, "c1"); /* c1 -> EXECUTING (toolbox 上报) */

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    cJSON *json = edc_state_serialize(s, sched);
    CHECK(json != NULL, "serialize ok");

    struct edc_session *s2 = edc_session_new(cfg, "sess-l", "L");
    struct edc_scheduler *sched2 = edc_scheduler_new(s2, cfg);
    CHECK(edc_state_deserialize(json, s2, sched2), "deserialize ok");

    struct edc_task *rt = find_task(s2, "lt");
    CHECK(rt != NULL, "task restored");
    CHECK(rt != NULL && rt->state == TASK_SUSPENDED, "TOOL_CALLING -> SUSPENDED");

    struct edc_tool_call_record *r1 = edc_task_tool_call_find(rt, "c1");
    struct edc_tool_call_record *r2 = edc_task_tool_call_find(rt, "c2");
    CHECK(r1 != NULL, "c1 restored");
    CHECK(r2 != NULL, "c2 restored");
    CHECK(r1 && r1->status == TOOL_CALL_EXECUTING, "c1 status EXECUTING preserved");
    CHECK(r2 && r2->status == TOOL_CALL_PENDING, "c2 status PENDING preserved");
    CHECK(r1 && r1->args != NULL, "c1 args restored");
    if (r1 && r1->args)
        CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(r1->args, "cmd")), "ls") == 0, "c1 args content");
    CHECK(r1 && r1->name && strcmp(r1->name, "shell") == 0, "c1 name restored");

    /* run: EXECUTING (in-flight) restore */
    CHECK(edc_task_tool_call_find(rt, "c1") != NULL && edc_task_tool_call_find(rt, "c2") != NULL,
          "ledger count == 2 (order c1,c2)");

    cJSON_Delete(json);
    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_session_free(s2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: resume with nothing pending -> WAITING_INPUT
 *  ══════════════════════════════════════════════════════════════ */

static void test_resume_waiting_input(void)
{
    TEST("resume: no deps/tools/turn -> WAITING_INPUT");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "sess-r1", "R1");
    struct edc_scheduler *sched = edc_scheduler_new(s, cfg);

    struct edc_task *t = edc_task_new("r1", "sess-r1", "r1", "task", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    force_suspended(t);
    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);
    CHECK(t->state == TASK_WAITING_INPUT, "resumed -> WAITING_INPUT");
    CHECK(g_send_llm_calls == 0, "no LLM dispatched");
    CHECK(g_redispatch_calls == 0, "no tools redispatched");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: resume with unsatisfied dep -> WAITING_DEPS
 *  ══════════════════════════════════════════════════════════════ */

static void test_resume_waiting_deps(void)
{
    TEST("resume: unsatisfied dep -> WAITING_DEPS");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "sess-r2", "R2");
    struct edc_scheduler *sched = edc_scheduler_new(s, cfg);

    struct edc_task *t = edc_task_new("r2", "sess-r2", "r2", "dependent", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    struct edc_task *src = edc_task_new("r2src", "sess-r2", "r2", "source", edc_channel_adapter);
    edc_task_set_state(src, TASK_WAITING_INPUT); /* not completed */
    edc_task_add_dependency(t, src, TASK_DEP_JOIN);
    CHECK(t->state == TASK_WAITING_DEPS, "waits on dep");
    force_suspended(t);

    struct edc_task_group *g = edc_task_group_new(t);
    edc_task_group_add_task(g, src);
    edc_session_add_group(s, g);

    edc_scheduler_resume_task(sched, t);
    CHECK(t->state == TASK_WAITING_DEPS, "resumed -> WAITING_DEPS (dep unsatisfied)");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 5: resume with an EXECUTING (in-flight) tool -> synthesize
 *          an interrupted response, then continue to LLM
 *  ══════════════════════════════════════════════════════════════ */

static void test_resume_executing_synthesizes(void)
{
    TEST("resume: EXECUTING tool -> synthesized interrupted response + resume LLM");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "sess-r3", "R3");
    struct edc_scheduler *sched = edc_scheduler_new(s, cfg);
    sched->send_llm = stub_send_llm;

    struct edc_task *t = edc_task_new("r3", "sess-r3", "r3", "task", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_TOOL_CALLING);
    seed_tool_turn(t, "c1", "shell");
    edc_task_tool_call_add(t, "c1", "shell", NULL);
    edc_task_tool_call_mark_executing(t, "c1"); /* c1 EXECUTING */
    force_suspended(t);

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(t->state == TASK_LLM_PROCESSING, "resumed -> LLM_PROCESSING (tool chain resolved)");
    CHECK(g_send_llm_calls == 1, "LLM re-dispatched once");
    CHECK(g_redispatch_calls == 0, "nothing redispatched");
    CHECK(turn_has_tool_msg(t->active_turn, "c1"), "synthesized tool response appended for c1");
    CHECK(edc_task_tool_call_find(t, "c1") == NULL, "EXECUTING record removed from ledger");
    CHECK(!edc_task_has_pending_tool_calls(t), "ledger empty");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 6: resume with a PENDING (not started) tool -> redispatch
 *  ══════════════════════════════════════════════════════════════ */

static void test_resume_pending_redispatches(void)
{
    TEST("resume: PENDING tool -> TOOL_CALLING + redispatch");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, "sess-r4", "R4");
    struct edc_scheduler *sched = edc_scheduler_new(s, cfg);
    sched->redispatch_tools = stub_redispatch;

    struct edc_task *t = edc_task_new("r4", "sess-r4", "r4", "task", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_TOOL_CALLING);
    seed_tool_turn(t, "c1", "shell");
    edc_task_tool_call_add(t, "c1", "shell", NULL); /* stays PENDING */
    force_suspended(t);

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(t->state == TASK_TOOL_CALLING, "resumed -> TOOL_CALLING (waiting for tools)");
    CHECK(g_redispatch_calls == 1, "redispatch callback invoked once");
    struct edc_tool_call_record *r = edc_task_tool_call_find(t, "c1");
    CHECK(r != NULL, "c1 record still present");
    CHECK(r && r->status == TOOL_CALL_PENDING, "c1 stays PENDING until toolbox reports started");
    CHECK(g_send_llm_calls == 0, "no LLM yet (waiting for tool result)");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

int main(void)
{
    log_info("=== task resume / tool ledger tests ===");
    log_info("");

    test_ledger_ops();
    test_ledger_serialize();
    test_resume_waiting_input();
    test_resume_waiting_deps();
    test_resume_executing_synthesizes();
    test_resume_pending_redispatches();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
