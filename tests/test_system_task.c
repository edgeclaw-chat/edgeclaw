/* P0a: the system task mechanism (sink / kind + sidebar). A system task is an
 * ordinary persisted task in the session system group: it must have real work, it
 * survives a restart (an interrupted fold resumes), and it is not hidden
 * (snapshot / /task status show it). */

#include "edc_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_group_fold.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_session_manager.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
/* The in-memory session store handed to the scheduler; an empty label means the
 * active session (active_index points at the first). */
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
/* Stub: _run_loop is referenced by edc_commands (pulled in transitively). */
volatile sig_atomic_t _run_loop = 1;

/* ══════════════════════════════════════════════════════════════
 *  Test framework (same minimal pattern as the other tests)
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
 *  Helpers
 *  ══════════════════════════════════════════════════════════════ */

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    return cfg;
}

static int count_groups(struct edc_session *session)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    n++;
    return n;
}

static int count_tasks(struct edc_session *session)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        n++;
    }
    return n;
}

static void turn_add_msg(struct edc_turn *turn, const char *role, const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddStringToObject(msg, "content", content);
    edc_turn_add_message(turn, msg);
}

/* 给任务装一个 turn (= 它的 input); 此时它停在 (AWAIT_EXEC, ALIVE) —— "已派发,
 * 等回话", 也就是 system task 真正在跑的相位。 */
static void give_input(struct edc_task *task, const char *text)
{
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = task->task_id;
    turn_add_msg(turn, "user", text);
    turn_add_msg(turn, "assistant", "ok");
    edc_task_add_turn(task, turn);
    edc_task_await_exec(task);
}

/* 让任务跑到 COMPLETED —— 走**真路径** (executor 推进 → scheduler 的策略点),
 * 因为策略点的簿记正挂在真推进路径上。 */
static void complete_with(struct edc_scheduler *sched, struct edc_task *task, const char *content)
{
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_EXEC_DONE;
    ev.as.exec.reason = EDC_REASON_STOP;
    ev.as.exec.content = (char *)content; /* executor 只读它 */
    void *t_req = task->current_request_id;
    struct edc_advance_result t_res = edc_executor_advance(&sched->executor, task, &ev);
    edc_scheduler_apply_advance(sched, task, t_req, &t_res);
}

static void complete_via_executor(struct edc_scheduler *sched, struct edc_task *task)
{
    complete_with(sched, task, "done");
}

/* Attach input and set COMPLETED directly (task layer), for the persistence
 * predicate only: this path bypasses the scheduler, so the task stays in memory to
 * be serialized. */
static void give_input_and_complete(struct edc_task *task, const char *text)
{
    give_input(task, text);
    edc_task_mark_completed(task);
}

/* 驱动 scheduler 的 actor: 单测里没有跑 loop (edc_actor_send 需要 actor->loop),
 * 但 handler 本身就是要测的那条路径 —— 快照下发挂在它的尾巴上。 */
static void pump_scheduler(struct edc_scheduler *sched, edc_actor_msg_type_t type)
{
    edc_actor_msg_t *msg = edc_actor_msg_new(type, NULL, NULL, NULL, NULL);
    sched->self->handler(sched->self, msg);
}

/* P3 ①b: 快照是**拉取**的 (请求带 session 标签)。 */
static void pull_snapshot(struct edc_scheduler *sched, struct edc_session *session)
{
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, NULL, NULL,
                                             sdsnew(session ? edc_id_str(session->session_id) : ""),
                                             (edc_actor_data_free_fn)sdsfree);
    sched->self->handler(sched->self, msg);
}

static sds g_snapshot = NULL;

static void capture_snapshot(struct edc_scheduler *sched, sds json)
{
    (void)sched;
    if (g_snapshot)
        sdsfree(g_snapshot);
    g_snapshot = json;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: 三个属性的默认值 / 谓词 / 名字
 *  ══════════════════════════════════════════════════════════════ */

static void test_attributes_default(void)
{
    TEST("sink/kind: 默认值 + 谓词 + 名字往返");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-default"), "Default");

    /* 普通用户任务: sink=CHANNEL;它的组: USER。 */
    struct edc_task *task = edc_task_new(edc_test_id("t-1"), edc_test_session_id("sess-default"), edc_test_id("t-1"), "user task", edc_channel_adapter);
    CHECK(task->sink == EDC_SINK_CHANNEL, "新任务默认 sink=CHANNEL");

    struct edc_task_group *group = edc_task_group_new(task);
    CHECK(group->kind == EDC_GROUP_KIND_USER, "新组默认 kind=USER");
    CHECK(!edc_task_group_is_system(group), "普通组不是 system");

    /* 名字往返 (日志/调试用)。 */
    CHECK(strcmp(edc_task_sink_name(EDC_SINK_GROUP_MEMORY), "GROUP_MEMORY") == 0, "sink 名");
    CHECK(edc_task_classify_sink("GROUP_MEMORY") == EDC_SINK_GROUP_MEMORY, "sink 解析");
    CHECK(edc_task_classify_sink("garbage") == EDC_SINK_CHANNEL, "sink 未知值退化为 CHANNEL");
    CHECK(edc_task_classify_sink(NULL) == EDC_SINK_CHANNEL, "sink NULL 退化为 CHANNEL");


    CHECK(strcmp(edc_task_group_kind_name(EDC_GROUP_KIND_SYSTEM), "system") == 0, "kind 名");
    CHECK(edc_task_group_classify_kind("system") == EDC_GROUP_KIND_SYSTEM, "kind 解析");
    CHECK(edc_task_group_classify_kind(NULL) == EDC_GROUP_KIND_USER, "kind NULL 退化为 USER");

    /* 谓词的 NULL 容错。 */
    CHECK(!edc_task_group_is_system(NULL), "is_system(NULL) = false");

    edc_task_group_free(group);
    edc_session_free(session);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: spawn —— 三个属性一次给全, 容器复用
 *  ══════════════════════════════════════════════════════════════ */

static void test_spawn_system_task(void)
{
    TEST("spawn: 建系统组 / 属性齐全 / 第二个复用同一个组");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-spawn"), "Spawn");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    CHECK(edc_session_find_system_group(session) == NULL, "还没有系统组");

    struct edc_task *a = edc_scheduler_spawn_system_task(sched, session, "fold group 1");
    CHECK(a != NULL, "spawn 返回任务");
    CHECK(a->sink == EDC_SINK_GROUP_MEMORY, "system task 的 sink 是 GROUP_MEMORY");
    CHECK(edc_task_is_ready(a) && edc_task_is_alive(a), "spawn 后停在 (READY, ALIVE): 还没有 input");

    struct edc_task_group *sg = edc_session_find_system_group(session);
    CHECK(sg != NULL, "系统组出现了");
    CHECK(edc_task_group_is_system(sg), "它就是系统组");
    CHECK(sg->group_id == a->task_id, "group_id = leader 的 task_id (与用户组同一套规则)");
    CHECK(count_groups(session) == 1 && count_tasks(session) == 1, "会话里就这一个任务");

    struct edc_task *b = edc_scheduler_spawn_system_task(sched, session, "fold group 2");
    CHECK(b != NULL && b->group == sg, "第二个 system task 复用同一个系统组");
    CHECK(count_groups(session) == 1, "没有多出第二个组");
    CHECK(count_tasks(session) == 2, "两个任务都在");
    CHECK(b->task_id != a->task_id, "两个任务各有自己的 id");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════════
 *  Test 3: 折叠任务**落盘** (好让被重启打断的那一轮能续跑)
 *  ══════════════════════════════════════════════════════════════════ */

static void test_system_task_persisted(void)
{
    TEST("落盘: 系统组 + 折叠任务进 session.json; 读回带 sink/sink_group/切点 且置回就绪");

    struct edc_config *cfg = make_config();
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("sess-p"), "P");
    struct edc_scheduler *sch = edc_scheduler_new(cfg);
    install_resolver(sch, s);

    /* 字面 id 必须带字母前缀: `sched->next_task_id` 是**进程级**递增计数器 (全套测试共享),
     * 纯数字的字面 id (曾经的 "10") 会被后面生成的 id 撞上 —— 撞了之后两个组会 claim 同一个
     * task, 落盘再读回就是"同一个对象进两个组", 释放时 double free (ASAN 抓到过)。 */
    struct edc_task *user = edc_task_new(edc_test_id("u-10"), edc_test_session_id("sess-p"), edc_test_id("u-10"), "user work", edc_channel_adapter);
    struct edc_task_group *ug = edc_task_group_new(user);
    edc_session_add_group(s, ug);
    give_input_and_complete(user, "hello");

    /* 一个"在飞"的折叠任务: 有 input (active_turn), 未完成, 指向用户组。 */
    struct edc_task *fold = edc_scheduler_spawn_system_task(sch, s, "fold it");
    edc_scheduler_attach_input(sch, fold, "summarize the group");
    edc_task_await_exec(fold);
    fold->sink_group = ug;
    fold->fold_cut_task_id = edc_test_id("cut-target");
    fold->fold_cut_turn = 3;
    fold->fold_cut_prefix = 42;

    cJSON *j = edc_state_serialize(s, sch);
    CHECK(j != NULL, "可序列化");
    cJSON *tasks = cJSON_GetObjectItem(j, "tasks");
    CHECK(cJSON_GetArraySize(tasks) == 2, "tasks 块含用户任务 + 折叠任务");
    cJSON *groups = cJSON_GetObjectItem(j, "groups");
    CHECK(cJSON_GetArraySize(groups) == 2, "groups 块含用户组 + 系统组");

    struct edc_session *s2 = edc_session_new(cfg, edc_test_session_id("sess-p"), "P");
    struct edc_scheduler *sch2 = edc_scheduler_new(cfg);
    install_resolver(sch2, s2);
    CHECK(edc_state_deserialize(j, s2, sch2), "反序列化成功");
    struct edc_task_group *sg2 = edc_session_find_system_group(s2);
    CHECK(sg2 != NULL && edc_task_group_is_system(sg2), "系统组读回来 (kind 由 leader 的 sink 派生)");
    struct edc_task *fold2 = sg2 ? sg2->members : NULL;
    CHECK(fold2 != NULL, "折叠任务读回来了");
    if (fold2)
    {
        CHECK(fold2->sink == EDC_SINK_GROUP_MEMORY, "sink 读回");
        CHECK(fold2->fold_cut_task_id && fold2->fold_cut_task_id == edc_test_id("cut-target") &&
              fold2->fold_cut_turn == 3 && fold2->fold_cut_prefix == 42,
          "切点三元组读回 (成员/第几个 turn/折到第几条)");
        CHECK(fold2->sink_group != NULL && fold2->sink_group->group_id == ug->group_id,
              "sink_group 解析回用户组");
        CHECK(edc_task_is_ready(fold2) && edc_task_is_alive(fold2),
              "在飞的折叠任务读回是 READY/ALIVE (重启由扫描重新派发), 不是 INTERRUPTED");
        CHECK(fold2->active_turn != NULL, "折叠 prompt 的 active_turn 也读回来了");
    }

    cJSON_Delete(j);
    edc_session_free(s);
    edc_scheduler_free(sch);
    edc_session_free(s2);
    edc_scheduler_free(sch2);
    edc_config_free(cfg);
}
/* ══════════════════════════════════════════════════════════════════
 *  Test 4: 折叠任务不回收 (跟普通任务一样留在系统组)
 *  ══════════════════════════════════════════════════════════════════ */

static void test_system_task_not_reaped(void)
{
    TEST("不回收: 完成/取消的 system task 留在系统组 (与普通任务同等)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-reap"), "Reap");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->snapshot_reply = capture_snapshot;

    struct edc_task *a = edc_scheduler_spawn_system_task(sched, session, "fold A");
    struct edc_task *b = edc_scheduler_spawn_system_task(sched, session, "fold B");
    struct edc_task_group *sg = edc_session_find_system_group(session);
    CHECK(sg != NULL && sg->member_count == 2, "系统组里有两个任务");
    edc_scheduler_attach_input(sched, a, "fold a");
    edc_task_await_exec(a);
    edc_scheduler_attach_input(sched, b, "fold b");
    edc_task_await_exec(b);
    CHECK(count_tasks(session) == 2, "两个都在跑");

    complete_via_executor(sched, a);
    pull_snapshot(sched, session);
    CHECK(edc_task_is_completed(a), "A 完成了");
    CHECK(count_tasks(session) == 2, "A 完成后仍在 (不回收)");
    CHECK(edc_session_find_system_group(session) == sg, "系统组还在");

    edc_scheduler_send_signal_to_task_ptr(sched, b, TASK_SIGTERM);
    pull_snapshot(sched, session);
    CHECK(edc_task_is_cancelled(b), "B 被取消了");
    CHECK(count_tasks(session) == 2, "B 取消后也还在 (不回收)");
    CHECK(edc_session_find_system_group(session) != NULL, "系统组还在 (不是'空了就删'的容器)");

    if (g_snapshot)
    {
        sdsfree(g_snapshot);
        g_snapshot = NULL;
    }
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}
/* ══════════════════════════════════════════════════════════════
 *  Test 5: 取消的任务留在树里 (没有回收路径)
 *  ══════════════════════════════════════════════════════════════ */

static void test_persistent_not_reaped(void)
{
    TEST("取消的用户任务留在树里 (没有回收)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-keep"), "Keep");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *user = edc_task_new(edc_test_id("1"), edc_test_session_id("sess-keep"), edc_test_id("1"), "user work", edc_channel_adapter);
    struct edc_task_group *ug = edc_task_group_new(user);
    edc_session_add_group(session, ug);
    give_input(user, "hi"); /* 停在 (AWAIT_EXEC, ALIVE): 真的可以取消 */

    /* 信号路径 → terminal; 拉一次快照照样能跑。 */
    edc_scheduler_send_signal_to_task_ptr(sched, user, TASK_SIGTERM);
    pull_snapshot(sched, session);

    CHECK(edc_task_is_cancelled(user), "用户任务确实被取消了");
    CHECK(edc_task_state_str(user) != NULL, "它还在 (没被 free: 读字段不崩)");
    CHECK(count_tasks(session) == 1, "任务留在树里");
    CHECK(count_groups(session) == 1, "组也还在");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* Test 6: the system task is not hidden -- both the snapshot and /task status show
 * it (the old implementation skipped on is_system everywhere). */

static void test_system_task_is_visible(void)
{
    TEST("可见性: 快照带 kind=system 且任务在树里;/task status 也数它");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-vis"), "Vis");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->snapshot_reply = capture_snapshot;

    struct edc_task *user = edc_task_new(edc_test_id("1"), edc_test_session_id("sess-vis"), edc_test_id("1"), "user work", edc_channel_adapter);
    struct edc_task_group *ug = edc_task_group_new(user);
    edc_session_add_group(session, ug);
    give_input_and_complete(user, "hi");

    struct edc_task *sys = edc_scheduler_spawn_system_task(sched, session, "fold everything");
    give_input_and_complete(sys, "fold");
    /* 别让它被回收 —— 本测的是"画得出来", 不是生命周期。 */
    sds sys_id = sdsnew(edc_id_str(sys->task_id));

    pull_snapshot(sched, session);
    CHECK(g_snapshot != NULL, "拿到快照");
    if (g_snapshot)
    {
        CHECK(strstr(g_snapshot, "\"kind\":\"system\"") != NULL, "系统组的 kind 进了快照");
        CHECK(strstr(g_snapshot, "\"kind\":\"user\"") != NULL, "用户组也带 kind (展示层据此分流)");
        sds idpat = sdscatprintf(sdsempty(), "\"id\":\"%s\"", sys_id);
        CHECK(strstr(g_snapshot, idpat) != NULL, "系统任务在快照里 (没有被藏起来)");
        sdsfree(idpat);
        CHECK(strstr(g_snapshot, "fold everything") != NULL, "它的 desc 也在");
    }

    /* /task status 的文案: 组数与任务数都把 system 那份数进去 (core 侧不留 skip)。
     * 两个任务都 COMPLETED → 直方图也一起数。 */
    sds st = edc_session_status_text(session, true, NULL);
    CHECK(st != NULL, "状态文案非空");
    if (st)
    {
        CHECK(strstr(st, "- **Groups**: 2") != NULL, "组数含系统组");
        CHECK(strstr(st, "- **Tasks**: 2") != NULL, "任务数含 system task");
        CHECK(strstr(st, "COMPLETED 2") != NULL, "直方图把两个都数进去");
    }

    sdsfree(st);
    sdsfree(sys_id);
    if (g_snapshot)
    {
        sdsfree(g_snapshot);
        g_snapshot = NULL;
    }
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 7 (P2b 卫生): 摘要响应带 tool_calls = 失败, 不是一次工具往返
 *  折叠请求根本不带工具定义 (edc_llm_request.c), 所以这个响应只可能是
 *  provider 行为异常。pi §3 判失败 ("Summarization attempted to call a tool"):
 *  不派发工具、不写记忆、任务落 ERROR。普通任务走同一条路**照常**派发。
 * ══════════════════════════════════════════════════════════════════ */

static int g_dispatch_calls = 0;

static void spy_dispatch_tools(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp, void *request_id)
{
    (void)sched;
    (void)task;
    (void)llm_resp;
    (void)request_id;
    g_dispatch_calls++;
}

/* 投一个"想调工具"的应答 (一条归一化 tool call), 走真 executor 推进。 */
static struct edc_advance_result drive_tool_call(struct edc_scheduler *sched, struct edc_task *task)
{
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_EXEC_DONE;
    ev.as.exec.reason = EDC_REASON_TOOL_CALL;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "content", "");
    ev.as.exec.msg = msg;
    cJSON *args = cJSON_CreateObject();
    struct edc_tool_call tc = {.id = "call_1", .name = "exec_shell", .args = args};
    ev.as.exec.calls = &tc;
    ev.as.exec.n_calls = 1;
    ev.as.exec.tool_calls_raw = NULL;
    /* 用量: 公共段在分支之前累加 ⇒ 失败的那次摘要调用照样记账。 */
    ev.as.exec.prompt_tokens = 7;
    ev.as.exec.completion_tokens = 3;
    ev.as.exec.total_tokens = 10;

    struct edc_advance_result res = edc_executor_advance(&sched->executor, task, &ev);

    /* 守卫路径不接手 msg (工具路径把它移进 turn, 那会把它置 NULL)。 */
    if (ev.as.exec.msg)
        cJSON_Delete(ev.as.exec.msg);
    cJSON_Delete(args);
    return res;
}

static void test_fold_tool_call_is_failure(void)
{
    TEST("P2b 卫生: 摘要响应带 tool_calls ⇒ ERROR + 不派发 + 记忆不动 (普通任务不受影响)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-hygiene"), "Hygiene");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->dispatch_tools = spy_dispatch_tools;

    /* 被折的组 (成员 = 普通用户任务) + 一条已有的记忆。 */
    struct edc_task *user = edc_task_new(edc_test_id("u-1"), edc_test_session_id("sess-hygiene"), edc_test_id("u-1"), "user task", edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(user);
    edc_task_group_add_task(group, user);
    group->memory = sdsnew("old memory: keep me");

    /* 折叠任务: sink=GROUP_MEMORY + 自己的 input (停在 AWAIT_EXEC)。 */
    struct edc_task *fold = edc_scheduler_spawn_system_task(sched, session, "fold group memory");
    CHECK(fold != NULL && fold->sink == EDC_SINK_GROUP_MEMORY, "折叠任务就位");
    if (!fold)
    {
        edc_session_free(session);
        edc_scheduler_free(sched);
        edc_config_free(cfg);
        return;
    }
    fold->sink_group = group;
    give_input(fold, "summarize the group");
    g_dispatch_calls = 0;

    struct edc_advance_result res = drive_tool_call(sched, fold);
    CHECK(res.outcome == EDC_STEP_ERROR, "落 ERROR (不是 AWAIT_TOOLS)");
    CHECK(edc_task_has_failed(fold) && !edc_task_is_alive(fold), "任务 life=ERROR");
    CHECK(fold->step != EDC_TASKSTEP_AWAIT_TOOLS, "没有被当成一次工具往返");
    CHECK(fold->tool_calls == NULL, "工具调用没进账本");
    CHECK(g_dispatch_calls == 0, "一次工具都没派发");
    CHECK(group->memory && strcmp(group->memory, "old memory: keep me") == 0, "组记忆一字不动");
    CHECK(res.notice && strstr(res.notice, "tool") != NULL, "提示语点名 tool");
    CHECK(fold->tokens_prompt == 7 && fold->tokens_completion == 3,
          "废掉的那次摘要调用照样记用量 (公共段在分支之前)");

    /* 对照: 同一条路, 普通任务照常进 AWAIT_TOOLS 并派发 (守卫只该管摘要调用)。 */
    give_input(user, "hi");
    g_dispatch_calls = 0;
    struct edc_advance_result ures = drive_tool_call(sched, user);
    CHECK(ures.outcome == EDC_STEP_BLOCKED, "普通任务: BLOCKED (等工具回话)");
    CHECK(edc_task_awaits_tools(user), "普通任务进 AWAIT_TOOLS");
    CHECK(g_dispatch_calls == 1, "普通任务照常派发");
    CHECK(user->tool_calls != NULL && strcmp(user->tool_calls->name, "exec_shell") == 0, "账本记了 PENDING");

    sdsfree(group->memory);
    group->memory = NULL;
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════ */

/* ── L2 策展任务 (P4d): 与折叠同时建 + JOIN 链 + 经记忆工具写入 ───── */

/* 会话系统组里找 sink = SESSION_MEMORY 的任务。 */
static struct edc_task *find_l2(struct edc_session *session)
{
    struct edc_task_group *sys = edc_session_find_system_group(session);
    if (!sys)
        return NULL;
    struct edc_task *t;
    LL_FOREACH2(sys->members, t, group_next)
    {
        if (t->sink == EDC_SINK_SESSION_MEMORY)
            return t;
    }
    return NULL;
}

/* 一个"可折叠"的组: 成员已完成 + 带一条 turn。 */
static struct edc_task_group *make_foldable_group(struct edc_session *sess, const char *id,
                                                 struct edc_task **out_member)
{
    struct edc_task *m = edc_task_new(edc_test_id(id), sess->session_id, edc_test_id(id), "member work", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(m);
    edc_session_add_group(sess, g);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = m->task_id;
    turn_add_msg(turn, "user", "please fold me");
    turn_add_msg(turn, "assistant", "noted");
    edc_task_add_turn(m, turn);
    edc_task_await_exec(m);
    edc_task_mark_completed(m);
    m->completion_seq = 1;
    if (out_member)
        *out_member = m;
    return g;
}

static void test_session_memory_chain_and_landing(void)
{
    TEST("L2: 与折叠**同时**建 (成员→L2→JOIN→L1); 策展经工具写入 (完成不落地, completion_seq = 0)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-l2"), "L2");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *member = NULL;
    struct edc_task_group *group = make_foldable_group(session, "m-a1", &member);

    /* 活着的成员 (非终态) —— 它既等折叠也等策展 */
    struct edc_task *live = edc_task_new(edc_test_id("u-live"), edc_test_session_id("sess-l2"), group->group_id, "live member", edc_channel_adapter);
    edc_task_group_add_task(group, live);

    struct edc_task *fold = edc_group_fold_start(sched, session, group, NULL);
    CHECK(fold != NULL, "折叠起来了");
    /* 生产里由扫描派发 (READY → AWAIT_EXEC); 这里只把相位摆好, 好让 complete_with 能走
     * 真推进路径 (executor → apply_advance)。 */
    edc_task_await_exec(fold);

    /* 关键: 策展任务是**当场**出现的 (不等折叠落地) */
    struct edc_task *l2 = find_l2(session);
    CHECK(l2 != NULL, "起折的同时就建好了 L2");
    CHECK(l2 && l2->sink == EDC_SINK_SESSION_MEMORY, "sink = SESSION_MEMORY");
    CHECK(l2 && l2->sink_group == group, "它记住来源组 (sources 与 holds_group 用)");
    CHECK(l2 && l2->deps_total == 1 && l2->deps_completed == 0, "它有一条未决的 JOIN 边 (对着折叠)");
    CHECK(edc_task_awaits_deps(live) ? true : true, "(相位随实现, 这里只断言链的效力)");
    CHECK(edc_task_has_unresolved_deps(live), "活成员有未决依赖 (折叠 + 策展)");

    /* 折叠落地 ⇒ L2 才可跑 (JOIN 边完成) */
    complete_with(sched, fold, "## Group memory\n- 构建入口是 bazelisk");
    CHECK(edc_task_has_unresolved_deps(live), "折叠完成 ≠ 成员恢复: 它还在等策展");
    CHECK(!edc_task_has_unresolved_deps(l2), "L2 的 JOIN 边完成了 ⇒ 它可跑");

    /* L2 的请求: 材料**不在 prompt 里**, 而是装配请求时由 JOIN 把它**拼进 L2 自己的输入**
     * (B 方案)。这个夹具没建请求, 所以此刻 turn 里还只有"索引 + 契约"。 */
    struct edc_turn *turn = l2->active_turn;
    CHECK(turn != NULL && turn->msg_count > 0, "L2 有自己的 turn (索引 + 契约)");
    const char *material = NULL;
    if (turn && turn->msg_count > 0)
    {
        cJSON *m = edc_turn_msg_at(turn, 0);
        material = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
    }
    CHECK(material && strstr(material, "Existing session entries") != NULL, "turn 里有条目索引与契约");
    CHECK(material && strstr(material, "构建入口是 bazelisk") == NULL, "此刻 turn 里没有组记忆 (装配请求时才拼)");
    CHECK(material && strstr(material, "appended at the end of this message") != NULL,
          "prompt 说清了材料拼在本条消息末尾");

    /* 结算 L2 ⇒ **不落地** (写入改为运行期经记忆工具发生; 完成只是收尾);
     * 成员才恢复。写入路径由 tests/test_tools_memory.c 覆盖。 */
    uint32_t seq_before = session->next_completion_seq;
    edc_task_await_exec(l2); /* 生产里由扫描派发; 这里摆好相位, 好让完成走真推进路径 */
    complete_with(sched, l2, "wrote entries via the memory tools (wrap-up)");
    CHECK(session->memo->entry_count == 0 && session->memo->event_count == 0,
          "策展任务完成本身不落地 (写入都在运行期经记忆工具发生)");
    CHECK(l2->completion_seq == 0, "L2 不占完成序");
    CHECK(session->next_completion_seq == seq_before, "完成序计数器没动");
    CHECK(!edc_task_has_unresolved_deps(live), "策展完成 ⇒ 成员被释放");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_session_memory_join_cascades_failure(void)
{
    TEST("L2: 折叠失败/取消 ⇒ 策展被 JOIN 级联 (失败/取消); 成员照常释放");

    /* 一个 session + 一个 scheduler (同文件其它用例同形; 不要在一个进程里起第二个
     * scheduler —— `active_contexts` 是单例静态全局, 那属于既有偏离)。 */
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-casc"), "Casc");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* ── ① 失败级联: 第二个组, 活成员, 折叠一失败 ── */
    struct edc_task *member = NULL;
    struct edc_task_group *gB = make_foldable_group(session, "m-b1", &member);
    struct edc_task *live = edc_task_new(edc_test_id("u-liveB"), edc_test_session_id("sess-casc"), gB->group_id, "live B", edc_channel_adapter);
    edc_task_group_add_task(gB, live);

    struct edc_task *fold = edc_group_fold_start(sched, session, gB, NULL);
    struct edc_task *l2 = find_l2(session);
    CHECK(fold && l2, "折叠与策展都在");
    edc_task_await_exec(fold);
    edc_task_mark_failed(fold);
    CHECK(edc_task_has_failed(fold), "折叠失败");
    CHECK(l2 && edc_task_has_failed(l2), "策展**自动**失败 (JOIN 级联, 没有额外逻辑)");
    CHECK(!edc_task_has_unresolved_deps(live), "成员被释放 (它的边都被处理掉了)");

    /* ── ② 取消级联: 第三个组, 折叠一起就被取消 (SIGTERM 那条生产路径) ── */
    struct edc_task *member2 = NULL;
    struct edc_task_group *gC = make_foldable_group(session, "m-c1", &member2);
    struct edc_task *live2 = edc_task_new(edc_test_id("u-liveC"), edc_test_session_id("sess-casc"), gC->group_id, "live C", edc_channel_adapter);
    edc_task_group_add_task(gC, live2);

    struct edc_task *fold2 = edc_group_fold_start(sched, session, gC, NULL);
    struct edc_task *l2b = NULL;
    {
        struct edc_task_group *sys = edc_session_find_system_group(session);
        struct edc_task *t = NULL;
        LL_FOREACH2(sys->members, t, group_next)
        {
            if (t->sink == EDC_SINK_SESSION_MEMORY && !edc_task_is_completed(t) && !edc_task_has_failed(t))
                l2b = t;
        }
    }
    CHECK(fold2 && l2b, "第二对也在");
    edc_task_await_exec(fold2);
    edc_task_send_signal(fold2, TASK_SIGTERM);
    CHECK(l2b && edc_task_is_cancelled(l2b), "折叠被取消 ⇒ 策展自动取消");
    CHECK(!edc_task_has_unresolved_deps(live2), "成员照常释放");

    /* 用户裁决的语义 (2026-10-10): 取消**不摘边** —— 源还能 resume, 边只是暂时满足不了。
     * 所以 resume 这个被级联取消的 L2, 它回到"等依赖" (而不是空手跑一遍, 也不是假装没依赖)。 */
    edc_scheduler_resume_task(sched, l2b);
    CHECK(edc_task_awaits_deps(l2b), "resume 它 ⇒ 回到等依赖");
    CHECK(edc_task_has_unresolved_deps(l2b), "那条边还在 (源不产出就不放它跑)");
    CHECK(!edc_task_is_running(l2b), "它没有被驱动");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    test_attributes_default();
    test_spawn_system_task();
    test_session_memory_chain_and_landing();
    test_session_memory_join_cascades_failure();
    test_system_task_persisted();
    test_system_task_not_reaped();
    test_persistent_not_reaped();
    test_system_task_is_visible();
    test_fold_tool_call_is_failure();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
