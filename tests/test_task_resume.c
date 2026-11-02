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
 * 注: 加载后停在 INTERRUPTED 的任务与用户 stop 的任务在状态机里是同一组出口，故测试用
 * force_interrupted() 直接置位模拟"从盘上读回一个停下来的任务"。
 */

#include "edc_channel_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include "log.h"
#include <cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
/* ── 单测的 session 仓库 ─────────────────────────────────────────
 * scheduler 现在**同包直调 store** (P3 的 `resolve_session` 端口已退休),
 * 所以测试给它的不是"解析函数"而是**仓库**: 一个只装着
 * 这些 session 的内存 manager (普通结构体, 没有构造函数 —— 与 poke 别的结构体同法)。
 * 标签为空 = 用活跃的那个 (active_index 指向第一个), 与真 store 的策略一致。
 * `ctx` 仍留给测试自己用 (它不再是读口的入参)。 */
static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}
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

#define TEST_WS edc_test_workspace()

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
            if (t->task_id == edc_test_id(task_id))
                return t;
        }
    }
    return NULL;
}

/* 模拟"加载后 INTERRUPTED": 绕过状态机直接把 life 落下 (落盘读回也不自己写第二套)。
 * 注意只动 life —— 中断不改"在哪一步"。 */
static void force_interrupted(struct edc_task *t)
{
    edc_task_force_move(t, t->step, EDC_LIFE_INTERRUPTED);
}

static bool turn_has_tool_msg(struct edc_turn *turn, const char *call_id)
{
    if (!turn)
        return false;
    for (int i = 0; i < turn->msg_count; i++)
    {
        cJSON *m = edc_turn_msg_at(turn, i);
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
    struct edc_task *t = edc_task_new(edc_test_id("t1"), edc_test_session_id("s"), edc_test_id("g"), "task", edc_channel_adapter);

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
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-l"), "L");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);

    struct edc_task *t = edc_task_new(edc_test_id("lt"), edc_test_session_id("sess-l"), edc_test_id("lt"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    edc_task_await_tools(t);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "cmd", "ls");
    edc_task_tool_call_add(t, "c1", "shell", args);
    edc_task_tool_call_add(t, "c2", "read_file", NULL);
    cJSON_Delete(args);
    edc_task_tool_call_mark_executing(t, "c1"); /* c1 -> EXECUTING (toolbox 上报) */

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    /* Through the store: there is no document to serialize any more, and flush/load is
     * what a restart actually does. */
    CHECK(edc_state_store_flush(s, sched), "flush ok");

    struct edc_session *s2 = edc_session_new(cfg, edc_test_session_id("sess-l"), "L");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_store_load(s2, sched2), "load ok");

    struct edc_task *rt = find_task(s2, "lt");
    CHECK(rt != NULL, "task restored");
    CHECK(rt != NULL && edc_task_is_interrupted(rt), "TOOL_CALLING -> INTERRUPTED");

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
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-r1"), "R1");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);

    struct edc_task *t = edc_task_new(edc_test_id("r1"), edc_test_session_id("sess-r1"), edc_test_id("r1"), "task", edc_channel_adapter);
    force_interrupted(t);
    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);
    CHECK(edc_task_is_ready(t), "resumed -> READY");
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
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-r2"), "R2");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);

    struct edc_task *t = edc_task_new(edc_test_id("r2"), edc_test_session_id("sess-r2"), edc_test_id("r2"), "dependent", edc_channel_adapter);
    struct edc_task *src = edc_task_new(edc_test_id("r2src"), edc_test_session_id("sess-r2"), edc_test_id("r2"), "source", edc_channel_adapter);
    edc_task_add_dependency(t, src, TASK_DEP_JOIN);
    CHECK(edc_task_awaits_deps(t), "waits on dep");
    force_interrupted(t);

    struct edc_task_group *g = edc_task_group_new(t);
    edc_task_group_add_task(g, src);
    edc_session_add_group(s, g);

    edc_scheduler_resume_task(sched, t);
    CHECK(edc_task_awaits_deps(t), "resumed -> WAITING_DEPS (dep unsatisfied)");

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
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-r3"), "R3");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->send_llm = stub_send_llm;

    struct edc_task *t = edc_task_new(edc_test_id("r3"), edc_test_session_id("sess-r3"), edc_test_id("r3"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    edc_task_await_tools(t);
    seed_tool_turn(t, "c1", "shell");
    edc_task_tool_call_add(t, "c1", "shell", NULL);
    edc_task_tool_call_mark_executing(t, "c1"); /* c1 EXECUTING */
    force_interrupted(t);

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(edc_task_awaits_exec(t), "resumed -> LLM_PROCESSING (tool chain resolved)");
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
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-r4"), "R4");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->redispatch_tools = stub_redispatch;

    struct edc_task *t = edc_task_new(edc_test_id("r4"), edc_test_session_id("sess-r4"), edc_test_id("r4"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    edc_task_await_tools(t);
    seed_tool_turn(t, "c1", "shell");
    edc_task_tool_call_add(t, "c1", "shell", NULL); /* stays PENDING */
    force_interrupted(t);

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(edc_task_awaits_tools(t), "resumed -> TOOL_CALLING (waiting for tools)");
    CHECK(g_redispatch_calls == 1, "redispatch callback invoked once");
    struct edc_tool_call_record *r = edc_task_tool_call_find(t, "c1");
    CHECK(r != NULL, "c1 record still present");
    CHECK(r && r->status == TOOL_CALL_PENDING, "c1 stays PENDING until toolbox reports started");
    CHECK(g_send_llm_calls == 0, "no LLM yet (waiting for tool result)");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ── 迁移过来的任务: 相位丢了 (step=READY), 残留状态才是线索 ──────────
 *
 * v4 的 SUSPENDED 任务, 磁盘上**根本没写**它当时在哪一步 —— 迁移只能给中性值
 * READY (by the one-shot migration that is no longer shipped)。如果 resume **只**看 step, 这类
 * 任务就会走进 READY 分支、把工具账本和 active_turn 全忽略掉: 排队的工具永远不
 * 重发, 任务静默卡死。
 *
 * 所以规矩定成: step 权威, 但 **READY 退回旧探测** —— v5 里真正 READY 的任务没有
 * 任何残留状态, 探测落到 WAITING_INPUT, 与今天一致。
 *
 * 这两条在批 3 之前就该绿 (今天 resume 压根不看 step), 批 3 之后必须**仍然**绿。
 * 它们是"迁移数据不被新逻辑改变行为"的守卫 —— 那一批唯一的风险就在这儿。 */
static void test_resume_migrated_task_with_pending_tool(void)
{
    TEST("resume: migrated task (step=READY) with a PENDING tool still redispatches");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-m1"), "M1");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->redispatch_tools = stub_redispatch;

    struct edc_task *t = edc_task_new(edc_test_id("m1"), edc_test_session_id("sess-m1"), edc_test_id("m1"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    edc_task_await_tools(t);
    seed_tool_turn(t, "c1", "shell");
    edc_task_tool_call_add(t, "c1", "shell", NULL); /* stays PENDING */
    force_interrupted(t);
    /* 迁移的产物: 相位丢了, 只有中性值 */
    t->step = EDC_TASKSTEP_READY;
    t->life = EDC_LIFE_INTERRUPTED;

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(edc_task_awaits_tools(t), "still rediscovers the tool via the ledger");
    CHECK(g_redispatch_calls == 1, "redispatch callback invoked once");
    CHECK(g_send_llm_calls == 0, "no LLM yet (waiting for tool result)");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_resume_migrated_task_with_turn(void)
{
    TEST("resume: migrated task (step=READY) with an open turn still resumes the LLM");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-m2"), "M2");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->send_llm = stub_send_llm;

    struct edc_task *t = edc_task_new(edc_test_id("m2"), edc_test_session_id("sess-m2"), edc_test_id("m2"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    seed_tool_turn(t, "c9", "shell"); /* 有 turn, 但账本里没有待办 */
    force_interrupted(t);
    t->step = EDC_TASKSTEP_READY; /* 迁移的产物 */
    t->life = EDC_LIFE_INTERRUPTED;

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(edc_task_awaits_exec(t), "the open turn is continued");
    CHECK(g_send_llm_calls == 1, "LLM resent exactly once");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ── 没有"等输入"这种相位 ────────────────────────────────────────────
 *
 * task **创建时就带着它的 input** (/task new <desc> 的 desc 就是 input; /task fork 同理)。
 * 所以一个 resume 时没有依赖、没有账本、也没有
 * turn 的任务, 不是"在等用户输入" —— 没有这种相位, 它只是**空闲** (READY)。
 *
 * 今天它落到 WAITING_INPUT: 那个值来自旧模型 (任务是个长期会话, 会坐在那儿等下一
 * 条消息), 后来只被三个"派不出去, 避免卡死"的死胡同当作兜底用。 */
static void test_resume_nothing_left_is_ready(void)
{
    TEST("resume: nothing to continue -> READY (there is no 'waiting for input' phase)");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-n1"), "N1");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);

    struct edc_task *t = edc_task_new(edc_test_id("n1"), edc_test_session_id("sess-n1"), edc_test_id("n1"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    force_interrupted(t);
    /* 没有依赖、没有工具账本、没有 active_turn —— 没有任何东西可以续 */

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    edc_scheduler_resume_task(sched, t);

    CHECK(t->step == EDC_TASKSTEP_READY, "lands on READY");
    CHECK(t->life == EDC_LIFE_ALIVE, "and it is alive");
    CHECK(edc_task_is_ready(t), "the mirror reads IDLE, not WAITING_INPUT");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 7: resume an ERROR task -> continue the failed turn
 *  ══════════════════════════════════════════════════════════════ */

static void test_resume_error_continues_turn(void)
{
    TEST("resume ERROR: continues the failed turn (reuses active_turn)");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-r5"), "R5");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->send_llm = stub_send_llm;

    struct edc_task *t = edc_task_new(edc_test_id("r5"), edc_test_session_id("sess-r5"), edc_test_id("r5"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    /* 失败前的半成品: 这轮已有 user 消息, LLM 请求失败没产出 assistant。 */
    t->active_turn = edc_turn_new();
    edc_turn_add_user_message(t->active_turn, "do the thing");
    edc_task_mark_failed(t); /* LLM_PROCESSING → ERROR */
    CHECK(edc_task_has_failed(t), "task is ERROR");

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(edc_task_awaits_exec(t), "resumed ERROR -> LLM_PROCESSING");
    CHECK(g_send_llm_calls == 1, "LLM re-dispatched once");
    CHECK(g_redispatch_calls == 0, "nothing redispatched");
    CHECK(t->active_turn != NULL, "active_turn preserved (continue, not restart)");
    CHECK(t->active_turn && t->active_turn->msg_count == 1,
          "half-finished round still intact");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 8: resume an ERROR task that still has a pending tool call
 *          -> TOOL_CALLING + redispatch (ERROR → TOOL_CALLING now valid)
 *  ══════════════════════════════════════════════════════════════ */

static void test_resume_error_pending_tool_redispatches(void)
{
    TEST("resume ERROR: pending tool -> TOOL_CALLING + redispatch");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-r6"), "R6");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->redispatch_tools = stub_redispatch;

    struct edc_task *t = edc_task_new(edc_test_id("r6"), edc_test_session_id("sess-r6"), edc_test_id("r6"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    edc_task_await_tools(t);
    seed_tool_turn(t, "c1", "shell");
    edc_task_tool_call_add(t, "c1", "shell", NULL);
    edc_task_mark_failed(t); /* TOOL_CALLING → ERROR */
    CHECK(edc_task_has_failed(t), "task is ERROR");

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(edc_task_awaits_tools(t), "resumed ERROR -> TOOL_CALLING");
    CHECK(g_redispatch_calls == 1, "redispatch callback invoked once");
    CHECK(t->active_turn != NULL, "active_turn preserved");
    CHECK(g_send_llm_calls == 0, "no LLM yet (waiting for tool result)");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 12: resume a CANCELLED task whose tool call was in flight
 *           (取消会清空账本 —— 悬空响应只能按**消息**补出来)
 *  ══════════════════════════════════════════════════════════════ */

static void test_resume_cancelled_dangling_tool(void)
{
    TEST("resume CANCELLED: dangling tool_call (ledger cleared by cancel) -> note + resume LLM");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-r7"), "R7");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->send_llm = stub_send_llm;

    struct edc_task *t = edc_task_new(edc_test_id("r7"), edc_test_session_id("sess-r7"), edc_test_id("r7"), "task", edc_channel_adapter);
    edc_task_await_exec(t);
    edc_task_await_tools(t);
    seed_tool_turn(t, "c1", "shell");
    edc_task_tool_call_add(t, "c1", "shell", NULL);
    edc_task_tool_call_mark_executing(t, "c1");
    edc_task_send_signal(t, TASK_SIGTERM); /* 真取消 (与 /task stop 同一条路径) */

    CHECK(edc_task_is_cancelled(t), "task is CANCELLED");
    CHECK(t->step == EDC_TASKSTEP_DONE, "cancel压平了 step (所以续跑必须重判)");
    CHECK(edc_task_tool_call_find(t, "c1") == NULL, "cancel cleared the ledger (消息是唯一线索)");
    CHECK(!turn_has_tool_msg(t->active_turn, "c1"), "no tool response yet");
    CHECK(edc_task_is_resumable(t), "CANCELLED is resumable (命令门)");

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    g_redispatch_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(edc_task_awaits_exec(t), "resumed CANCELLED -> LLM_PROCESSING");
    CHECK(g_send_llm_calls == 1, "LLM re-dispatched once");
    CHECK(g_redispatch_calls == 0, "the interrupted tool is not re-run");
    CHECK(turn_has_tool_msg(t->active_turn, "c1"), "interrupted tool response synthesized from the message");

    /* 幂等: 再 resume 一次不会补第二条 */
    edc_scheduler_resume_task(sched, t);
    size_t tool_msgs = 0;
    for (size_t i = 0; i < t->active_turn->msg_count; i++)
    {
        cJSON *m = edc_turn_msg_at(t->active_turn, i);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "tool") == 0)
            tool_msgs++;
    }
    CHECK(tool_msgs == 1, "second resume adds no duplicate tool response");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 13: resume a CANCELLED task with nothing to continue
 *  ══════════════════════════════════════════════════════════════ */

static void test_resume_cancelled_nothing_left(void)
{
    TEST("resume CANCELLED: no turn/tools -> READY + ALIVE, no LLM");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-r8"), "R8");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->send_llm = stub_send_llm;

    struct edc_task *t = edc_task_new(edc_test_id("r8"), edc_test_session_id("sess-r8"), edc_test_id("r8"), "task", edc_channel_adapter);
    edc_task_send_signal(t, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(t), "task is CANCELLED");

    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g_send_llm_calls = 0;
    edc_scheduler_resume_task(sched, t);

    CHECK(t->step == EDC_TASKSTEP_READY, "lands on READY");
    CHECK(t->life == EDC_LIFE_ALIVE, "and it is alive");
    CHECK(g_send_llm_calls == 0, "nothing to continue -> no LLM request");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* 一个因为依赖没产出而结束的任务，output 里留着级联写的那句
 * "[failed: its dependency … ]"。resume 撤销的正是那个状态，所以那句机器文案必须跟着走，
 * 否则 /task output 会报一个正在重试的"失败"。 */
static void test_resume_blocked_clears_the_note(void)
{
    TEST("resume: a BLOCKED task drops the cascade's note from its output");
    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-blocked"), "Blocked");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);
    sched->send_llm = stub_send_llm;

    struct edc_task *src = edc_task_new(edc_test_id("bl-src"), edc_test_session_id("sess-blocked"), edc_test_id("bl-src"), "src", edc_channel_adapter);
    struct edc_task *dep = edc_task_new(edc_test_id("bl-dep"), edc_test_session_id("sess-blocked"), edc_test_id("bl-src"), "dep", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(src);
    edc_session_add_group(s, g);
    edc_task_group_add_task(g, dep);
    edc_task_add_dependency(dep, src, TASK_DEP_SEQUENCE);
    edc_task_await_exec(src);
    edc_task_mark_interrupted(src); /* 级联: dep 变 BLOCKED，并写下那句 note */

    CHECK(dep->end == EDC_END_BLOCKED && dep->output != NULL, "夹具: 被阻塞且带着 note");
    edc_scheduler_resume_task(sched, dep);
    CHECK(dep->output == NULL, "note 被清掉 (它描述的状态已经撤销)");
    CHECK(edc_task_awaits_deps(dep), "并且停在 dep-wait: 源还没产出");
    CHECK(dep->end == EDC_END_NONE, "原因也清了");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== task resume / tool ledger tests ===");
    log_info("");

    test_ledger_ops();
    test_ledger_serialize();
    test_resume_waiting_input();
    test_resume_waiting_deps();
    test_resume_executing_synthesizes();
    test_resume_pending_redispatches();
    test_resume_migrated_task_with_pending_tool();
    test_resume_migrated_task_with_turn();
    test_resume_nothing_left_is_ready();
    test_resume_error_continues_turn();
    test_resume_error_pending_tool_redispatches();
    test_resume_cancelled_dangling_tool();
    test_resume_cancelled_nothing_left();
    test_resume_blocked_clears_the_note();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
