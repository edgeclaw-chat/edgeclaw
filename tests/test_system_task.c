/* P0a: the system task mechanism (sink / kind + sidebar). A system task is an
 * ordinary persisted task in the session system group: it must have real work, it
 * survives a restart (an interrupted fold resumes), and it is not hidden
 * (snapshot / /task status show it). */

#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
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
    cfg->workspace = sdsnew("/tmp/test_system_task_workspace");
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
    edc_queue_push(&turn->messages, msg);
}

/* 给任务装一个 turn (= 它的 input); 此时它停在 (AWAIT_EXEC, ALIVE) —— "已派发,
 * 等回话", 也就是 system task 真正在跑的相位。 */
static void give_input(struct edc_task *task, const char *text)
{
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(task->task_id);
    turn_add_msg(turn, "user", text);
    turn_add_msg(turn, "assistant", "ok");
    edc_task_add_turn(task, turn);
    edc_task_await_exec(task);
}

/* 让任务跑到 COMPLETED —— 走**真路径** (executor 推进 → scheduler 的策略点),
 * 因为策略点的簿记正挂在真推进路径上。 */
static void complete_via_executor(struct edc_scheduler *sched, struct edc_task *task)
{
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_EXEC_DONE;
    ev.as.exec.reason = EDC_REASON_STOP;
    ev.as.exec.content = "done";
    void *t_req = task->current_request_id;
    struct edc_advance_result t_res = edc_executor_advance(&sched->executor, task, &ev);
    edc_scheduler_apply_advance(sched, task, t_req, &t_res);
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
                                             sdsnew(session ? session->session_id : ""),
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
    struct edc_session *session = edc_session_new(cfg, "sess-default", "Default");

    /* 普通用户任务: sink=CHANNEL;它的组: USER。 */
    struct edc_task *task = edc_task_new("t-1", "sess-default", "t-1", "user task", edc_channel_adapter);
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
    struct edc_session *session = edc_session_new(cfg, "sess-spawn", "Spawn");
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
    CHECK(strcmp(sg->group_id, a->task_id) == 0, "group_id = leader 的 task_id (与用户组同一套规则)");
    CHECK(count_groups(session) == 1 && count_tasks(session) == 1, "会话里就这一个任务");

    struct edc_task *b = edc_scheduler_spawn_system_task(sched, session, "fold group 2");
    CHECK(b != NULL && b->group == sg, "第二个 system task 复用同一个系统组");
    CHECK(count_groups(session) == 1, "没有多出第二个组");
    CHECK(count_tasks(session) == 2, "两个任务都在");
    CHECK(strcmp(b->task_id, a->task_id) != 0, "两个任务各有自己的 id");

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
    struct edc_session *s = edc_session_new(cfg, "sess-p", "P");
    struct edc_scheduler *sch = edc_scheduler_new(cfg);
    install_resolver(sch, s);

    struct edc_task *user = edc_task_new("10", "sess-p", "10", "user work", edc_channel_adapter);
    struct edc_task_group *ug = edc_task_group_new(user);
    edc_session_add_group(s, ug);
    give_input_and_complete(user, "hello");

    /* 一个"在飞"的折叠任务: 有 input (active_turn), 未完成, 指向用户组。 */
    struct edc_task *fold = edc_scheduler_spawn_system_task(sch, s, "fold it");
    edc_scheduler_attach_input(sch, fold, "summarize the group");
    edc_task_await_exec(fold);
    fold->sink_group = ug;
    fold->fold_cut_task_id = strdup("cut-target");
    fold->fold_cut_turn = 3;
    fold->fold_cut_prefix = 42;

    cJSON *j = edc_state_serialize(s, sch);
    CHECK(j != NULL, "可序列化");
    cJSON *tasks = cJSON_GetObjectItem(j, "tasks");
    CHECK(cJSON_GetArraySize(tasks) == 2, "tasks 块含用户任务 + 折叠任务");
    cJSON *groups = cJSON_GetObjectItem(j, "groups");
    CHECK(cJSON_GetArraySize(groups) == 2, "groups 块含用户组 + 系统组");

    struct edc_session *s2 = edc_session_new(cfg, "sess-p", "P");
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
        CHECK(fold2->fold_cut_task_id && strcmp(fold2->fold_cut_task_id, "cut-target") == 0 &&
              fold2->fold_cut_turn == 3 && fold2->fold_cut_prefix == 42,
          "切点三元组读回 (成员/第几个 turn/折到第几条)");
        CHECK(fold2->sink_group != NULL && strcmp(fold2->sink_group->group_id, ug->group_id) == 0,
              "sink_group 解析回用户组");
        CHECK(edc_task_is_ready(fold2) && edc_task_is_alive(fold2),
              "在飞的折叠任务读回是 READY/ALIVE (重启由扫描重新派发), 不是 SUSPENDED");
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
    struct edc_session *session = edc_session_new(cfg, "sess-reap", "Reap");
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
    struct edc_session *session = edc_session_new(cfg, "sess-keep", "Keep");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *user = edc_task_new("1", "sess-keep", "1", "user work", edc_channel_adapter);
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
    struct edc_session *session = edc_session_new(cfg, "sess-vis", "Vis");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->snapshot_reply = capture_snapshot;

    struct edc_task *user = edc_task_new("1", "sess-vis", "1", "user work", edc_channel_adapter);
    struct edc_task_group *ug = edc_task_group_new(user);
    edc_session_add_group(session, ug);
    give_input_and_complete(user, "hi");

    struct edc_task *sys = edc_scheduler_spawn_system_task(sched, session, "fold everything");
    give_input_and_complete(sys, "fold");
    /* 别让它被回收 —— 本测的是"画得出来", 不是生命周期。 */
    sds sys_id = sdsnew(sys->task_id);

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

/* ══════════════════════════════════════════════════════════════ */

int main(void)
{
    test_attributes_default();
    test_spawn_system_task();
    test_system_task_persisted();
    test_system_task_not_reaped();
    test_persistent_not_reaped();
    test_system_task_is_visible();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
