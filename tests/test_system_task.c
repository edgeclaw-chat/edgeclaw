/* ══════════════════════════════════════════════════════════════
 *  P0a: system task 机制 (sink / lifetime / kind + 侧栏)
 *
 *  runtime-model.md §4.6 的四条规则, 这里逐个钉住:
 *   ① 必须有活干 (不能是空容器)        → 这里不测行为, 测"它是个普通任务"
 *   ② TRANSIENT 不落盘、完成即回收      → test_transient_not_persisted / _reaped
 *   ③ 一个属性 = 一条规则 + 一个谓词   → 落盘 (lifetime) / 快照 kind (展示层)
 *   ④ 不进入被折叠 group 的 completion_seq → P0b 的折叠测试
 *
 *  最要紧的是那两条**反面**断言 (老 `Group system` 的两种死法):
 *    - 不落盘 ⇒ 盘上不可能有僵尸 (重启无残留);
 *    - **不隐藏** ⇒ 快照 / `/task status` 里都能看见它, 于是没有"漏一处特判"
 *      这回事 (老实现靠 is_system 在 5 处 skip, 漏一处就冒出来)。
 * ══════════════════════════════════════════════════════════════ */

#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
/* ── P3 ③ 测试读口 ──────────────────────────────────────────────
 * scheduler 不再持有"当前 session", 所以单测要给它装一个 store 读口: 把请求里
 * 的 session 标签解析成 session 对象 (入站路由 / 快照拉取都走它)。这一层测试
 * 没有 agent, ctx 就留给它 (agent 那层用它自己的读口)。 */
static struct edc_session *test_resolve_session(void *ctx, const char *session_id)
{
    struct edc_session *sess = ctx;
    if (!sess)
        return NULL;
    if (!session_id || !*session_id)
        return sess;
    return (sess->session_id && strcmp(session_id, sess->session_id) == 0) ? sess : NULL;
}

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    sched->resolve_session = test_resolve_session;
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

/* 让任务跑到 COMPLETED —— **走 scheduler 的写入口** (executor 的 set_task_state
 * 端口 → sched_move_task), 因为回收队列正是在那里入队的。 */
static void complete_via_executor(struct edc_scheduler *sched, struct edc_task *task)
{
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_EXEC_DONE;
    ev.as.exec.reason = EDC_REASON_STOP;
    ev.as.exec.content = "done";
    edc_executor_advance(&sched->executor, task, &ev);
}

/* 给任务装 input 并直接置成 COMPLETED (task 层)。
 * 只给"测落盘判据"用 —— 那条路径不经过 scheduler, 所以不会入回收队列, 任务会
 * 留在内存里等着被序列化 (真实系统里这一瞬间也存在: 折叠任务在跑的时候正好赶上
 * 一次落盘)。 */
static void give_input_and_complete(struct edc_task *task, const char *text)
{
    give_input(task, text);
    edc_task_mark_completed(task);
}

/* 驱动 scheduler 的 actor: 单测里没有跑 loop (edc_actor_send 需要 actor->loop),
 * 但 handler 本身就是要测的那条路径 —— 快照下发与回收结算都挂在它的尾巴上。 */
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
    TEST("sink/lifetime/kind: 默认值 + 谓词 + 名字往返");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-default", "Default");

    /* 普通用户任务: (CHANNEL, PERSISTENT);它的组: USER。 */
    struct edc_task *task = edc_task_new("t-1", "sess-default", "t-1", "user task", edc_channel_adapter);
    CHECK(task->sink == EDC_SINK_CHANNEL, "新任务默认 sink=CHANNEL");
    CHECK(task->lifetime == EDC_LIFETIME_PERSISTENT, "新任务默认 lifetime=PERSISTENT");
    CHECK(!edc_task_is_transient(task), "普通任务不是 transient");

    struct edc_task_group *group = edc_task_group_new(task);
    CHECK(group->kind == EDC_GROUP_KIND_USER, "新组默认 kind=USER");
    CHECK(!edc_task_group_is_system(group), "普通组不是 system");

    /* 名字往返 (日志/调试用; 落盘由 lifetime 的谓词回答)。 */
    CHECK(strcmp(edc_task_sink_name(EDC_SINK_GROUP_MEMORY), "GROUP_MEMORY") == 0, "sink 名");
    CHECK(edc_task_classify_sink("GROUP_MEMORY") == EDC_SINK_GROUP_MEMORY, "sink 解析");
    CHECK(edc_task_classify_sink("garbage") == EDC_SINK_CHANNEL, "sink 未知值退化为 CHANNEL");
    CHECK(edc_task_classify_sink(NULL) == EDC_SINK_CHANNEL, "sink NULL 退化为 CHANNEL");

    CHECK(strcmp(edc_task_lifetime_name(EDC_LIFETIME_TRANSIENT), "TRANSIENT") == 0, "lifetime 名");
    CHECK(edc_task_classify_lifetime("TRANSIENT") == EDC_LIFETIME_TRANSIENT, "lifetime 解析");
    CHECK(edc_task_classify_lifetime("garbage") == EDC_LIFETIME_PERSISTENT, "lifetime 未知值退化为 PERSISTENT");

    CHECK(strcmp(edc_task_group_kind_name(EDC_GROUP_KIND_SYSTEM), "system") == 0, "kind 名");
    CHECK(edc_task_group_classify_kind("system") == EDC_GROUP_KIND_SYSTEM, "kind 解析");
    CHECK(edc_task_group_classify_kind(NULL) == EDC_GROUP_KIND_USER, "kind NULL 退化为 USER");

    /* 谓词的 NULL 容错 (回收路径会拿 NULL 探针)。 */
    CHECK(!edc_task_is_transient(NULL), "is_transient(NULL) = false");
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
    CHECK(a->lifetime == EDC_LIFETIME_TRANSIENT, "system task 是 TRANSIENT");
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

/* ══════════════════════════════════════════════════════════════
 *  Test 3: TRANSIENT 不落盘 (盘上不可能有僵尸)
 *  ══════════════════════════════════════════════════════════════ */

static void test_transient_not_persisted(void)
{
    TEST("落盘: TRANSIENT 任务与它的组都不进 session.json");

    struct edc_config *cfg = make_config();

    /* A = 对照组: 只有一个用户组 + 一个已完成的任务。 */
    struct edc_session *sa = edc_session_new(cfg, "sess-p", "P");
    struct edc_scheduler *scha = edc_scheduler_new(cfg);
    install_resolver(scha, sa);
    struct edc_task *ua = edc_task_new("10", "sess-p", "10", "user work", edc_channel_adapter);
    struct edc_task_group *ga = edc_task_group_new(ua);
    edc_session_add_group(sa, ga);
    give_input_and_complete(ua, "hello");
    cJSON *ja = edc_state_serialize(sa, scha);
    CHECK(ja != NULL, "对照组可序列化");

    /* B = 同一份用户侧内容 + 一个系统组 (其任务是 COMPLETED, 已带 input ——
     * 也就是说: 只有 lifetime 这一关能拦住它)。 */
    struct edc_session *sb = edc_session_new(cfg, "sess-p", "P");
    struct edc_scheduler *schb = edc_scheduler_new(cfg);
    install_resolver(schb, sb);
    struct edc_task *ub = edc_task_new("10", "sess-p", "10", "user work", edc_channel_adapter);
    struct edc_task_group *gb = edc_task_group_new(ub);
    edc_session_add_group(sb, gb);
    give_input_and_complete(ub, "hello");

    struct edc_task *sysb = edc_scheduler_spawn_system_task(schb, sb, "fold it");
    give_input_and_complete(sysb, "summarize the group");
    sysb->completion_seq = 7;
    cJSON *jb = edc_state_serialize(sb, schb);
    CHECK(jb != NULL, "含系统组的会话可序列化");

    /* 任务块里没有它, 组块里也没有它的组。 */
    cJSON *tasks = cJSON_GetObjectItem(jb, "tasks");
    CHECK(cJSON_GetArraySize(tasks) == 1, "tasks 块只有用户任务");
    CHECK(cJSON_GetArrayItem(tasks, 0) != NULL, "第一个就是用户任务");
    cJSON *groups = cJSON_GetObjectItem(jb, "groups");
    CHECK(cJSON_GetArraySize(groups) == 1, "groups 块只有用户组");

    /* 最硬的一条: 与对照组的产物**逐字节相同** —— 系统组在盘上像没存在过。
     * (变异: 拿掉落盘判据里的 lifetime 那一关 → 这里立刻不同。) */
    char *ta = cJSON_PrintUnformatted(ja);
    char *tb = cJSON_PrintUnformatted(jb);
    CHECK(ta && tb && strcmp(ta, tb) == 0, "含系统组的产物与不含的逐字节相同");
    if (ta)
        cJSON_free(ta);
    if (tb)
        cJSON_free(tb);

    /* 读回来: 系统组不会复活 (它从来没进过文件)。 */
    struct edc_session *s2 = edc_session_new(cfg, "sess-p", "P");
    struct edc_scheduler *sc2 = edc_scheduler_new(cfg);
    install_resolver(sc2, s2);
    CHECK(edc_state_deserialize(jb, s2, sc2), "反序列化成功");
    CHECK(count_groups(s2) == 1, "读回 1 个组");
    CHECK(count_tasks(s2) == 1, "读回 1 个任务");
    CHECK(edc_session_find_system_group(s2) == NULL, "读回没有系统组 (重启无残留)");

    cJSON_Delete(ja);
    cJSON_Delete(jb);
    edc_session_free(sa);
    edc_scheduler_free(scha);
    edc_session_free(sb);
    edc_scheduler_free(schb);
    edc_session_free(s2);
    edc_scheduler_free(sc2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: 完成即回收;组空了就删
 *  ══════════════════════════════════════════════════════════════ */

static void test_transient_reaped(void)
{
    TEST("回收: 完成的 system task 消失;组空了连组一起删");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-reap", "Reap");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->snapshot_reply = capture_snapshot;

    /* 两个共用系统组的 system task。 */
    struct edc_task *a = edc_scheduler_spawn_system_task(sched, session, "fold A");
    struct edc_task *b = edc_scheduler_spawn_system_task(sched, session, "fold B");
    struct edc_task_group *sg = edc_session_find_system_group(session);
    CHECK(sg != NULL && sg->member_count == 2, "系统组里有两个任务");
    give_input(a, "fold a");
    give_input(b, "fold b");
    sds id_a = sdsdup(a->task_id);
    sds id_b = sdsdup(b->task_id);
    CHECK(count_tasks(session) == 2, "两个都在跑");

    /* (1) 完成 A —— 经 executor 的 set_task_state 端口入队, 消息末尾结算。 */
    complete_via_executor(sched, a);
    pull_snapshot(sched, session);
    CHECK(count_tasks(session) == 1, "A 被回收了");
    CHECK(count_groups(session) == 1, "组里还有 B, 所以组还在");
    CHECK(edc_session_find_system_group(session) == sg, "同一个组对象");

    /* (2) 取消 B —— 信号路径**绕开** sched_move_task (executor 直接改 task 状态),
     * 回收同样要覆盖它;此时组空了, 连组一起删。 */
    edc_scheduler_send_signal_to_task_ptr(sched, b, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(b), "B 被取消了");
    pull_snapshot(sched, session);
    CHECK(count_tasks(session) == 0, "B 也被回收了");
    CHECK(count_groups(session) == 0, "组空了就被删掉 (系统组不是容器)");
    CHECK(edc_session_find_system_group(session) == NULL, "会话里不再有系统组");
    CHECK(g_snapshot != NULL, "快照下发过一次");
    if (g_snapshot)
    {
        /* 快照里已经没有它们了 —— 回收发生在下发之前 (顺序反了的话这里会红)。
         * 匹配带引号的 id: 裸数字会在时间戳/计数里碰巧命中。 */
        sds pa = sdscatprintf(sdsempty(), "\"id\":\"%s\"", id_a);
        sds pb = sdscatprintf(sdsempty(), "\"id\":\"%s\"", id_b);
        CHECK(strstr(g_snapshot, pa) == NULL, "快照里不会出现已回收的 A");
        CHECK(strstr(g_snapshot, pb) == NULL, "快照里不会出现已回收的 B");
        sdsfree(pa);
        sdsfree(pb);
    }

    /* 再 spawn 一个: 没有系统组时重新建一个 (空了就删, 需要时再建)。 */
    struct edc_task *c = edc_scheduler_spawn_system_task(sched, session, "fold C");
    CHECK(c != NULL && edc_session_find_system_group(session) != NULL, "按需重建系统组");
    CHECK(count_groups(session) == 1, "仍然只有一个系统组");
    give_input(c, "fold c");
    edc_scheduler_send_signal_to_task_ptr(sched, c, TASK_SIGTERM);
    pull_snapshot(sched, session);
    CHECK(count_tasks(session) == 0 && count_groups(session) == 0, "重建的那个也被回收");

    sdsfree(id_a);
    sdsfree(id_b);
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
 *  Test 5: 只有 TRANSIENT 会被回收 (反面控制)
 *  ══════════════════════════════════════════════════════════════ */

static void test_persistent_not_reaped(void)
{
    TEST("回收只管 TRANSIENT: 用户任务取消后仍在树里");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-keep", "Keep");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *user = edc_task_new("1", "sess-keep", "1", "user work", edc_channel_adapter);
    struct edc_task_group *ug = edc_task_group_new(user);
    edc_session_add_group(session, ug);
    give_input(user, "hi"); /* 停在 (AWAIT_EXEC, ALIVE): 真的可以取消 */

    /* 信号路径 → terminal → 入回收队列; 结算点照样跑一遍。 */
    edc_scheduler_send_signal_to_task_ptr(sched, user, TASK_SIGTERM);
    pull_snapshot(sched, session);

    CHECK(edc_task_is_cancelled(user), "用户任务确实被取消了");
    CHECK(edc_task_state_str(user) != NULL, "它还在 (没被 free: 读字段不崩)");
    CHECK(count_tasks(session) == 1, "PERSISTENT 任务不被回收");
    CHECK(count_groups(session) == 1, "组也还在");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 6: **不隐藏** —— 快照与 /task status 都看得见系统任务
 *  (老实现靠 is_system 到处 skip;这里断言反面)
 *  ══════════════════════════════════════════════════════════════ */

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
    test_transient_not_persisted();
    test_transient_reaped();
    test_persistent_not_reaped();
    test_system_task_is_visible();

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
