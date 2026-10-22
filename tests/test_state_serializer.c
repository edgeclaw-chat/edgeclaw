#include "edc_state_serializer.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "edc_scheduler.h"
#include "edc_channel_actor.h"
#include "edc_config.h"
#include "edc_compat.h"
#include "utlist_fix.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
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
/* Stub: _run_loop is referenced by edc_commands (which gets pulled in
 * transitively).  Provide a dummy definition for test builds. */
volatile sig_atomic_t _run_loop = 1;

/* ══════════════════════════════════════════════════════════════
 *  Test framework (minimal, same pattern as other tests)
 *  ══════════════════════════════════════════════════════════════ */

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while(0)
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        log_error("    FAIL: %s", msg); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while(0)

#define CHECK_STREQ(a, b, msg) CHECK(strcmp((a) ? (a) : "", (b) ? (b) : "") == 0, msg)

/* ── Helper: create a minimal config ── */

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew("/tmp/test_edc_serializer_workspace");
    return cfg;
}

/* ── Helper: add a simple message to a turn ── */

static void turn_add_msg(struct edc_turn *turn, const char *role, const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddStringToObject(msg, "content", content);
    edc_queue_push(&turn->messages, msg);
}

/* ── Helper: count tasks in a session (across all groups) ── */

static int count_tasks(struct edc_session *session)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g) {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next) { n++; }
    }
    return n;
}

/* ── Helper: count groups ── */

static int count_groups(struct edc_session *session)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g) { n++; }
    return n;
}

/* ── Helper: find a task by id ── */

static struct edc_task *find_task(struct edc_session *session, const char *task_id)
{
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g) {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next) {
            if (strcmp(t->task_id, task_id) == 0) return t;
        }
    }
    return NULL;
}

/* ── Helper: find a group by id ── */

static struct edc_task_group *find_group(struct edc_session *session, const char *group_id)
{
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g) {
        if (strcmp(g->group_id, group_id) == 0) return g;
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: Empty session roundtrip
 *  ══════════════════════════════════════════════════════════════ */

static void test_empty_session(void)
{
    TEST("empty session roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-empty", "Empty");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* Verify initial state */
    CHECK(count_groups(session) == 0, "no groups initially");
    CHECK(count_tasks(session) == 0, "no tasks initially");
    CHECK(session->active_task == NULL, "no active task");

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");
    /* Verify JSON structure */
    CHECK(cJSON_GetObjectItem(json, "version") != NULL, "has version");
    CHECK(cJSON_GetObjectItem(json, "session") != NULL, "has session block");
    CHECK(cJSON_GetObjectItem(json, "groups") != NULL, "has groups block");
    CHECK(cJSON_GetObjectItem(json, "tasks") != NULL, "has tasks block");

    /* Deserialize into a fresh session */
    struct edc_session *session2 = edc_session_new(cfg, "sess-empty", "Empty");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    CHECK(count_groups(session2) == 0, "still no groups after deserialize");
    CHECK(count_tasks(session2) == 0, "still no tasks after deserialize");
    CHECK(session2->active_task == NULL, "no active task after deserialize");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: Single task (COMPLETED) with one turn
 *  ══════════════════════════════════════════════════════════════ */

static void test_single_task(void)
{
    TEST("single task COMPLETED with one turn");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-st", "SingleTask");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* Build a task with one completed turn */
    struct edc_task *task = edc_task_new("task-1", "sess-st", "task-1", "do something", edc_channel_adapter);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);
    task->is_group_leader = 1;
    task->output = sdsnew("task output result");
    task->completion_seq = 42;
    /* 用户原文 (由哪条输入建成): 可选键, 但要能原样来回。 */
    task->raw_input = sdsnew("do something");
    /* `{{…}}` 引用已收集、尚未展开：这个标志不落盘的话，重启后消息里就只剩字面量
     * （展开阶段被它 gate 住）——静默丢语义。 */
    task->has_deferred_vars = 1;

    /* Add a turn with messages */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-1");
    turn_add_msg(turn, "user", "hello");
    turn_add_msg(turn, "assistant", "world");
    edc_task_add_turn(task, turn);

    /* Create group, add task, add to session */
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);
    session->active_task = task;
    session->next_completion_seq = 43;

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    /* Check structure */
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    CHECK(cJSON_GetArraySize(tasks_j) == 1, "1 task serialized");

    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t0, "task_id")), "task-1", "task_id preserved");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t0, "life")), "COMPLETED", "life COMPLETED");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t0, "step")), "DONE", "step DONE");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(t0, "is_group_leader")), "is_group_leader");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t0, "raw_input")), "do something", "raw_input persisted");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(t0, "deferred_vars")), "deferred_vars persisted");

    cJSON *turns_j = cJSON_GetObjectItem(t0, "turns");
    CHECK(cJSON_GetArraySize(turns_j) == 1, "1 turn serialized");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-st", "SingleTask");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    CHECK(count_groups(session2) == 1, "1 group restored");
    CHECK(count_tasks(session2) == 1, "1 task restored");

    struct edc_task *rtask = find_task(session2, "task-1");
    CHECK(rtask != NULL, "task-1 found after deserialize");
    CHECK(edc_task_is_completed(rtask), "state remains COMPLETED");
    CHECK(rtask->is_group_leader == 1, "is_group_leader preserved");
    CHECK(rtask->completion_seq == 42, "completion_seq preserved");
    CHECK_STREQ(rtask->output, "task output result", "output preserved");
    CHECK_STREQ(rtask->raw_input, "do something", "raw_input restored");
    CHECK(rtask->has_deferred_vars == 1, "deferred_vars restored -- a {{…}} is still expanded after a restart");

    /* Check turn messages */
    CHECK(edc_queue_length(&rtask->turns) == 1, "1 turn in turns queue");
    /* Get the turn from the queue */
    struct edc_turn *rturn = (struct edc_turn *)rtask->turns.data[rtask->turns.head];
    CHECK(rturn != NULL, "turn is non-NULL");
    CHECK(edc_queue_length(&rturn->messages) == 2, "turn has 2 messages");

    /* Verify message content */
    cJSON *m0 = (cJSON *)rturn->messages.data[rturn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "role")), "user", "msg0 role=user");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "hello", "msg0 content");

    cJSON *m1 = (cJSON *)rturn->messages.data[rturn->messages.head + 1];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m1, "role")), "assistant", "msg1 role=assistant");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m1, "content")), "world", "msg1 content");

    CHECK(session2->active_task == rtask, "active_task restored");
    CHECK(session2->next_completion_seq == 43, "next_completion_seq preserved");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: Fork chain
 *  ══════════════════════════════════════════════════════════════ */

/* 新模型: task 创建时就带着它的 input —— 落盘判据会丢掉 "READY 但身上什么都没有"
 * 的 turn-less 残渣 (docs/retire-waiting-input.md §4), 所以 fixture 给成员装上 input。 */
static void give_input(struct edc_task *t, const char *text)
{
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(t->task_id);
    edc_turn_add_user_message(turn, text);
    t->active_turn = turn;
}

static void test_fork_chain(void)
{
    TEST("fork chain roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-fork", "Fork");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* Create parent task */
    struct edc_task *parent = edc_task_new("task-parent", "sess-fork", "task-parent",
                                            "parent task", edc_channel_adapter);
    parent->is_group_leader = 1;
    edc_task_mark_ready(parent);
    edc_task_await_exec(parent);
    edc_task_mark_completed(parent);

    /* Create fork children */
    struct edc_task *child1 = edc_task_fork(parent, "task-child1", "child 1", 10, edc_channel_adapter);
    edc_task_mark_ready(child1);
    edc_task_await_exec(child1);
    edc_task_mark_completed(child1);

    struct edc_task *child2 = edc_task_fork(parent, "task-child2", "child 2", 10, edc_channel_adapter);
    /* 新模型: fork 子任务创建时就带着它的 input —— /task fork 现在把 desc 当 input
     * 装上并派发。旧 fixture 让子任务停在 WAITING_INPUT 等
     * attach 喂输入, 那种 turn-less 任务现在落盘时会被丢掉
     * (docs/retire-waiting-input.md §4)。 */
    {
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(child2->task_id);
        edc_turn_add_user_message(turn, "child 2 input");
        child2->active_turn = turn;
        edc_task_mark_interrupted(child2);
    }

    /* Create group, add all tasks */
    struct edc_task_group *group = edc_task_group_new(parent);
    edc_task_group_add_task(group, child1);
    edc_task_group_add_task(group, child2);
    edc_session_add_group(session, group);

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    CHECK(cJSON_GetArraySize(tasks_j) == 3, "3 tasks serialized");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-fork", "Fork");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    CHECK(count_tasks(session2) == 3, "3 tasks restored");

    struct edc_task *rp  = find_task(session2, "task-parent");
    struct edc_task *rc1 = find_task(session2, "task-child1");
    struct edc_task *rc2 = find_task(session2, "task-child2");

    CHECK(rp != NULL, "parent found");
    CHECK(rc1 != NULL, "child1 found");
    CHECK(rc2 != NULL, "child2 found");

    /* Verify fork chain */
    CHECK(rc1->forked_from == rp, "child1->forked_from == parent");
    CHECK(rc2->forked_from == rp, "child2->forked_from == parent");
    CHECK(rp->forked_from == NULL, "parent has no forked_from");
    CHECK(rc1->fork_point_completion_seq == 10, "fork_point_completion_seq preserved");

    /* Verify states */
    CHECK(edc_task_is_completed(rp), "parent COMPLETED");
    CHECK(edc_task_is_completed(rc1), "child1 COMPLETED");
    CHECK(edc_task_is_interrupted(rc2), "child2 was in flight -> INTERRUPTED");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: Dependency graph
 *  ══════════════════════════════════════════════════════════════ */

static void test_dependency_graph(void)
{
    TEST("dependency graph roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-dep", "Dep");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* Create tasks: A → (JOIN) → B → (JOIN) → C */
    struct edc_task *ta = edc_task_new("task-A", "sess-dep", "task-A", "source A", edc_channel_adapter);
    ta->is_group_leader = 1;
    edc_task_mark_ready(ta);
    edc_task_await_exec(ta);
    edc_task_mark_completed(ta);
    ta->output = sdsnew("output from A");

    struct edc_task *tb = edc_task_new("task-B", "sess-dep", "task-A", "middle B", edc_channel_adapter);
    edc_task_mark_waiting_deps(tb); /* must be set before adding deps */
    /* 新模型: 任务创建时就带着它的 input。B 也是"带着输入"建出来的, 只是依赖先完成,
     * 于是它回到 READY (有 turn, 待驱动); 没有 turn 的 READY 任务会被落盘判据丢掉
     * (docs/retire-waiting-input.md §4)。 */
    {
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(tb->task_id);
        edc_turn_add_user_message(turn, "middle B input");
        tb->active_turn = turn;
    }

    struct edc_task *tc = edc_task_new("task-C", "sess-dep", "task-A", "sink C", edc_channel_adapter);
    edc_task_mark_waiting_deps(tc);

    /* Add dependencies: B depends on A, C depends on B */
    edc_task_add_dependency(tb, ta, TASK_DEP_JOIN);
    /* ta is completed, so dep should be auto-completed → B transitions to WAITING_INPUT */
    edc_task_add_dependency(tc, tb, TASK_DEP_SEQUENCE);
    /* tb not completed, so tc stays in WAITING_DEPS */

    /* Create group, add all */
    struct edc_task_group *group = edc_task_group_new(ta);
    edc_task_group_add_task(group, tb);
    edc_task_group_add_task(group, tc);
    edc_session_add_group(session, group);

    /* Verify deps setup before serialization */
    CHECK(tb->deps_total == 1, "B has 1 dep");
    CHECK(tb->deps_completed == 1, "B deps completed (A already done)");
    CHECK(edc_task_is_ready(tb), "B transitioned to READY");

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-dep", "Dep");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    CHECK(count_tasks(session2) == 3, "3 tasks restored");

    struct edc_task *ra = find_task(session2, "task-A");
    struct edc_task *rb = find_task(session2, "task-B");
    struct edc_task *rc = find_task(session2, "task-C");

    CHECK(ra != NULL, "A found");
    CHECK(rb != NULL, "B found");
    CHECK(rc != NULL, "C found");

    /* Verify dependencies */
    CHECK(rb->deps_total == 1, "B deps_total = 1");
    CHECK(rb->deps_completed == 1, "B deps_completed = 1");
    CHECK(edc_task_is_interrupted(rb), "B (deps satisfied) comes back INTERRUPTED on load");

    /* B came back INTERRUPTED, and an interrupt propagates like a failure -- but a task that
     * is itself INTERRUPTED cannot be cascaded into ERROR (the state machine only lets a
     * stopped task resume or cancel), so C stays INTERRUPTED with the edge kept: resuming C
     * returns it to dep-wait until B is resumed and produces. */
    CHECK(rc->deps_total == 1, "C deps_total stays 1 (the edge is kept)");
    CHECK(rc->deps_completed == 0, "C deps_completed = 0 (B produced nothing)");
    CHECK(edc_task_is_interrupted(rc) || edc_task_has_failed(rc),
          "C is stopped by the unsatisfiable edge (never released by it)");

    /* Verify dep types */
    struct edc_task_dep *dep_b = rb->deps;
    CHECK(dep_b != NULL, "B has dep node");
    CHECK(dep_b->source == ra, "B dep source = A");
    CHECK(dep_b->type == TASK_DEP_JOIN, "B dep type = JOIN");
    CHECK(dep_b->completed, "B dep completed");

    struct edc_task_dep *dep_c = rc->deps;
    CHECK(dep_c != NULL, "C has dep node");
    CHECK(dep_c->source == rb, "C dep source = B");
    CHECK(dep_c->type == TASK_DEP_SEQUENCE, "C dep type = SEQUENCE");
    CHECK(!dep_c->cancelled, "and that node is NOT voided");

    /* Verify output */
    CHECK_STREQ(ra->output, "output from A", "A output preserved");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 5: Complex topology (multi-group, multi-task, mixed)
 *  ══════════════════════════════════════════════════════════════ */

static void test_complex_topology(void)
{
    TEST("complex topology roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-cplx", "Complex");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* Group 1: 3 tasks */
    struct edc_task *g1_leader = edc_task_new("g1-L", "sess-cplx", "g1-L",
                                               "group1 leader", edc_channel_adapter);
    g1_leader->is_group_leader = 1;
    edc_task_mark_ready(g1_leader);
    edc_task_await_exec(g1_leader);
    edc_task_mark_completed(g1_leader);
    g1_leader->output = sdsnew("g1 leader done");

    struct edc_task *g1_m1 = edc_task_new("g1-M1", "sess-cplx", "g1-L",
                                           "group1 member1", edc_channel_adapter);
    give_input(g1_m1, "g1 member1 input");

    struct edc_task *g1_m2 = edc_task_new("g1-M2", "sess-cplx", "g1-L",
                                           "group1 member2", edc_channel_adapter);
    give_input(g1_m2, "g1 member2 input");

    struct edc_task_group *group1 = edc_task_group_new(g1_leader);
    edc_task_group_add_task(group1, g1_m1);
    edc_task_group_add_task(group1, g1_m2);
    edc_session_add_group(session, group1);

    /* Group 2: 2 tasks, normal group */
    struct edc_task *g2_leader = edc_task_new("g2-L", "sess-cplx", "g2-L",
                                               "group2 leader", edc_channel_adapter);
    g2_leader->is_group_leader = 1;
    edc_task_mark_ready(g2_leader);
    edc_task_await_exec(g2_leader);
    edc_task_mark_completed(g2_leader);

    struct edc_task *g2_m1 = edc_task_new("g2-M1", "sess-cplx", "g2-L",
                                           "group2 member1", edc_channel_adapter);
    give_input(g2_m1, "g2 member1 input");

    struct edc_task_group *group2 = edc_task_group_new(g2_leader);
    edc_task_group_add_task(group2, g2_m1);
    edc_session_add_group(session, group2);

    /* Cross-group dependency: g1-M1 depends on g2-M1 */
    edc_task_add_dependency(g1_m1, g2_m1, TASK_DEP_JOIN);

    /* Add turns to g1_leader */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("g1-L");
    turn_add_msg(turn, "user", "analyze");
    turn_add_msg(turn, "assistant", "done");
    edc_task_add_turn(g1_leader, turn);

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    /* READY 但**带着 input** 的任务照样落盘; 只有 "READY 但身上什么都没有" 的
     * turn-less 残渣会被丢掉 (见 edc_state_serializer.c 里的落盘判据)。 */
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    int tcount = cJSON_GetArraySize(tasks_j);
    CHECK(tcount == 5, "5 tasks serialized (READY with an input is persisted too)");

    /* Verify groups: 2 groups */
    cJSON *groups_j = cJSON_GetObjectItem(json, "groups");
    CHECK(cJSON_GetArraySize(groups_j) == 2, "2 groups serialized");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-cplx", "Complex");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    CHECK(count_groups(session2) == 2, "2 groups restored");
    CHECK(count_tasks(session2) == 5, "5 tasks restored (READY with an input persisted too)");

    struct edc_task *rg1L  = find_task(session2, "g1-L");
    struct edc_task *rg1M1 = find_task(session2, "g1-M1");
    struct edc_task *rg2L  = find_task(session2, "g2-L");
    struct edc_task *rg2M1 = find_task(session2, "g2-M1");
    struct edc_task *rg1M2 = find_task(session2, "g1-M2");

    CHECK(rg1L != NULL, "g1-L restored");
    CHECK(rg1M1 != NULL, "g1-M1 restored");
    CHECK(rg2L != NULL, "g2-L restored");
    CHECK(rg2M1 != NULL, "g2-M1 restored");
    CHECK(rg1M2 != NULL, "g1-M2 restored (READY with an input is persisted)");

    /* Verify cross-group dep */
    CHECK(rg1M1->deps_total == 1, "g1-M1 has 1 dep");
    CHECK(rg1M1->deps->source == rg2M1, "g1-M1 depends on g2-M1");

    /* Verify turn on g1-L */
    CHECK(edc_queue_length(&rg1L->turns) == 1, "g1-L has 1 turn");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 5b: 落盘判据 —— "READY 且没有 input" 的残渣不落盘
 *  ══════════════════════════════════════════════════════════════ */

static void test_turnless_shell_is_dropped(void)
{
    TEST("a READY task with no input is not persisted (no turn-less shells)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-shell", "Shell");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* 旧模型的残渣形状: 创建后不带 input 就停在 READY (fork leader / 历史 join 路径的
     * Joined task / @task 无正文)。新模型里任务创建时就带着它的 input, 所以这条形状
     * 不该存在 —— 落盘判据丢掉它并告警
     * (见 edc_state_serializer.c / docs/retire-waiting-input.md §4)。 */
    struct edc_task *shell = edc_task_new("t-shell", "sess-shell", "t-shell",
                                          "no input", edc_channel_adapter);
    shell->is_group_leader = 1;
    struct edc_task_group *group = edc_task_group_new(shell);
    edc_session_add_group(session, group);
    CHECK(edc_task_is_ready(shell), "shell starts READY");

    /* 对照组: 同样停在 READY, 但它带着 input → 必须落盘。
     * 放进它自己的 group 当 leader —— 被丢的 t-shell 是它那个 group 的 leader,
     * leader 消失后整个 group 也重建不出来, 会把同组的对照组一起带没。 */
    struct edc_task *real = edc_task_new("t-real", "sess-shell", "t-real",
                                         "has input", edc_channel_adapter);
    real->is_group_leader = 1;
    give_input(real, "real input");
    edc_session_add_group(session, edc_task_group_new(real));
    CHECK(edc_task_is_ready(real), "control task also sits at READY");

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    CHECK(cJSON_GetArraySize(tasks_j) == 1, "only the task carrying an input is persisted");

    struct edc_session *session2 = edc_session_new(cfg, "sess-shell", "Shell");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    CHECK(edc_state_deserialize(json, session2, sched2), "deserialize succeeds");
    CHECK(find_task(session2, "t-shell") == NULL, "the turn-less shell did not come back");
    CHECK(find_task(session2, "t-real") != NULL, "the task with an input came back");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 6: Interrupted-state normalization
 *  ══════════════════════════════════════════════════════════════ */

static void test_state_degradation(void)
{
    TEST("interrupted STEPS are preserved; only life collapses to INTERRUPTED");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-deg", "Degrade");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *t1 = edc_task_new("t-llm", "sess-deg", "t-llm",
                                        "llm processing", edc_channel_adapter);
    t1->is_group_leader = 1;
    edc_task_mark_ready(t1);
    edc_task_await_exec(t1);
    /* stays in LLM_PROCESSING */

    struct edc_task *t2 = edc_task_new("t-tool", "sess-deg", "t-llm",
                                        "tool calling", edc_channel_adapter);
    edc_task_mark_ready(t2);
    edc_task_await_exec(t2);
    edc_task_await_tools(t2);
    /* stays in TOOL_CALLING */
    /* `!cmd`: a task that only issues its seeded tool call. Must round-trip, or a
     * restart would resume it as an ordinary task and ask the model to continue. */
    t2->tool_only = true;

    struct edc_task_group *group = edc_task_group_new(t1);
    edc_task_group_add_task(group, t2);
    edc_session_add_group(session, group);

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    /* Verify states in JSON: should still be the original state names */
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *j1 = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *j2 = cJSON_GetArrayItem(tasks_j, 1);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(j1, "step")),
                "AWAIT_EXEC", "JSON keeps step AWAIT_EXEC");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(j2, "step")),
                "AWAIT_TOOLS", "JSON keeps step AWAIT_TOOLS");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(j2, "tool_only")), "JSON keeps tool_only");
    CHECK(!cJSON_GetObjectItem(j1, "tool_only"), "an ordinary task writes no tool_only key");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-deg", "Degrade");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt1 = find_task(session2, "t-llm");
    struct edc_task *rt2 = find_task(session2, "t-tool");

    /* P2 之前这里断言的是"相位被丢掉": 两个 task 都变成 INTERRUPTED, 分不出谁在哪一步。
     * 现在 life 仍然收敛 (重启后没有东西在跑), 但 **step 原样保留** —— 这正是能够让
     * resume 不再靠残留物猜的那份信息。 */
    CHECK(rt1->step == EDC_TASKSTEP_AWAIT_EXEC, "step AWAIT_EXEC preserved");
    CHECK(rt2->step == EDC_TASKSTEP_AWAIT_TOOLS, "step AWAIT_TOOLS preserved");
    CHECK(rt2->tool_only, "tool_only survives the round trip");
    CHECK(!rt1->tool_only, "an ordinary task stays ordinary");
    CHECK(rt1->life == EDC_LIFE_INTERRUPTED && rt2->life == EDC_LIFE_INTERRUPTED,
          "both were in flight -> INTERRUPTED");
    CHECK(rt1->step != rt2->step, "the two are still distinguishable");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 7: IDLE / CANCELLED filtering
 *  ══════════════════════════════════════════════════════════════ */

static void test_filter_transient_states(void)
{
    TEST("READY (carrying an input) is persisted too; CANCELLED is persisted");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-filt", "Filter");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *leader = edc_task_new("t-ok", "sess-filt", "t-ok",
                                            "good task", edc_channel_adapter);
    leader->is_group_leader = 1;
    edc_task_mark_ready(leader);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);

    struct edc_task *idle_t = edc_task_new("t-idle", "sess-filt", "t-ok",
                                            "idle task", edc_channel_adapter);
    give_input(idle_t, "idle input"); /* 停在 READY, 但带着 input */

    struct edc_task *canc_t = edc_task_new("t-canc", "sess-filt", "t-ok",
                                            "cancelled task", edc_channel_adapter);
    edc_task_mark_ready(canc_t);
    edc_task_mark_cancelled(canc_t);

    struct edc_task_group *group = edc_task_group_new(leader);
    edc_task_group_add_task(group, idle_t);
    edc_task_group_add_task(group, canc_t);
    edc_session_add_group(session, group);

    /* Sanity: 3 tasks in session */
    CHECK(count_tasks(session) == 3, "3 tasks before serialize");

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    CHECK(cJSON_GetArraySize(tasks_j) == 3, "3 tasks serialized (t-ok + t-idle + t-canc; READY with input too)");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-filt", "Filter");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");
    CHECK(count_tasks(session2) == 3, "3 tasks restored");

    struct edc_task *rcanc = find_task(session2, "t-canc");
    CHECK(rcanc != NULL, "cancelled task restored");
    CHECK(edc_task_is_cancelled(rcanc), "cancelled state survives restart");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 7b: CANCELLED mid-flight task roundtrip
 *  ══════════════════════════════════════════════════════════════ */

static void test_cancelled_midflight_roundtrip(void)
{
    TEST("CANCELLED mid-flight task (active_turn) roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-cmid", "CancelledMid");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* Task was running (LLM_PROCESSING) with a partial active turn, then got
     * a stop signal → CANCELLED.  Simulates /task stop during a run. */
    struct edc_task *task = edc_task_new("c-mid", "sess-cmid", "c-mid",
                                         "running then stopped", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);

    struct edc_turn *at = edc_turn_new();
    at->task_id = strdup("c-mid");
    turn_add_msg(at, "user", "please do the long task");
    task->active_turn = at;

    edc_task_send_signal(task, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(task), "signal puts task into CANCELLED");

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);
    session->active_task = task;

    /* Serialize: CANCELLED task must be present */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    CHECK(cJSON_GetArraySize(tasks_j) == 1, "CANCELLED task is serialized");
    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t0, "life")),
                "CANCELLED", "JSON keeps life CANCELLED");
    CHECK(cJSON_GetObjectItem(t0, "active_turn") != NULL, "active_turn serialized");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-cmid", "CancelledMid");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt = find_task(session2, "c-mid");
    CHECK(rt != NULL, "cancelled task restored");
    CHECK(edc_task_is_cancelled(rt), "state stays CANCELLED after restart (no WAITING_INPUT degrade)");
    CHECK(rt->active_turn != NULL, "active_turn restored");
    struct edc_turn *rturn = rt->active_turn;
    CHECK(edc_queue_length(&rturn->messages) == 1, "active turn has 1 message");
    cJSON *m0 = (cJSON *)rturn->messages.data[rturn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "role")), "user", "active turn msg role=user");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "please do the long task",
                "active turn msg content");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 7c: cancelled dependency edges survive roundtrip
 *  ══════════════════════════════════════════════════════════════ */

static void test_cancelled_dep_roundtrip(void)
{
    TEST("dependency edge of a cancelled source survives roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-cdep", "CancelledDep");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* src: group leader, runs and then gets cancelled.
     * tgt: SEQUENCE-depends on src; when src is cancelled only this dependency
     *      edge is dropped and tgt returns to WAITING_INPUT. */
    struct edc_task *src = edc_task_new("src", "sess-cdep", "src", "source task", edc_channel_adapter);
    src->is_group_leader = 1;
    edc_task_mark_ready(src);
    edc_task_await_exec(src);

    struct edc_task *tgt = edc_task_new("tgt", "sess-cdep", "src", "dependent task", edc_channel_adapter);
    /* 新模型: 任务创建时就带着它的 input —— tgt 被释放回 READY 时仍带着 turn,
     * 否则它会被落盘判据当成 turn-less 残渣丢掉 (docs/retire-waiting-input.md §4)。 */
    give_input(tgt, "dependent input");

    struct edc_task_group *group = edc_task_group_new(src);
    edc_task_group_add_task(group, tgt);
    edc_session_add_group(session, group);

    /* wire dep, then cancel the source (runtime order) */
    bool ok_add = edc_task_add_dependency(tgt, src, TASK_DEP_SEQUENCE);
    CHECK(ok_add, "add dependency succeeds");
    CHECK(edc_task_awaits_deps(tgt), "tgt waits on deps");
    edc_task_send_signal(src, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(src), "src cancelled");
    CHECK(edc_task_is_cancelled(tgt), "tgt cascaded to CANCELLED (SEQ behaves like JOIN)");

    /* Serialize: both tasks must be present */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    CHECK(cJSON_GetArraySize(tasks_j) == 2, "both src(CANCELLED) and tgt serialized");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-cdep", "CancelledDep");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rsrc = find_task(session2, "src");
    struct edc_task *rtgt = find_task(session2, "tgt");
    CHECK(rsrc != NULL && edc_task_is_cancelled(rsrc), "cancelled source restored as CANCELLED");
    CHECK(rtgt != NULL, "dependent restored");
    CHECK(edc_task_is_cancelled(rtgt), "dependent comes back CANCELLED too");
    CHECK(rtgt->deps_total == 1, "deps_total stays 1: the edge is kept");
    CHECK(rtgt->deps_completed == 0, "deps_completed 0");
    CHECK(rtgt->deps != NULL && !rtgt->deps->cancelled, "the dep edge is preserved, not voided");
    if (rtgt->deps)
    {
        CHECK(!rtgt->deps->cancelled, "dep edge NOT voided (JOIN/SEQ keep it)");
        CHECK(rtgt->deps->source == rsrc, "dep edge points at restored source");
    }

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 7d: JOIN cascade — cancelling a source cancels dependents
 *  ══════════════════════════════════════════════════════════════ */

static void test_join_cascade_roundtrip(void)
{
    TEST("JOIN cancel cascades the dependent to CANCELLED across roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-jcas", "JoinCascade");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *src = edc_task_new("jsrc", "sess-jcas", "jsrc", "source", edc_channel_adapter);
    src->is_group_leader = 1;
    edc_task_mark_ready(src);
    struct edc_task *tgt = edc_task_new("jtgt", "sess-jcas", "jsrc", "dependent", edc_channel_adapter);

    struct edc_task_group *group = edc_task_group_new(src);
    edc_task_group_add_task(group, tgt);
    edc_session_add_group(session, group);

    CHECK(edc_task_add_dependency(tgt, src, TASK_DEP_JOIN), "tgt -JOIN-> src");
    edc_task_send_signal(src, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(src), "src cancelled");
    CHECK(edc_task_is_cancelled(tgt), "JOIN dependent cascaded to CANCELLED");

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    struct edc_session *session2 = edc_session_new(cfg, "sess-jcas", "JoinCascade");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");
    struct edc_task *rtgt = find_task(session2, "jtgt");
    CHECK(rtgt != NULL && edc_task_is_cancelled(rtgt), "dependent restored as CANCELLED");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 8: Active turn roundtrip
 *  ══════════════════════════════════════════════════════════════ */

static void test_active_turn(void)
{
    TEST("active turn roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-at", "ActiveTurn");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new("t-at", "sess-at", "t-at",
                                          "has active turn", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);

    /* Add a committed turn */
    struct edc_turn *turn1 = edc_turn_new();
    turn1->task_id = strdup("t-at");
    turn_add_msg(turn1, "user", "q1");
    turn_add_msg(turn1, "assistant", "a1");
    edc_task_add_turn(task, turn1);

    /* Add an active turn (not yet committed) */
    struct edc_turn *active = edc_turn_new();
    active->task_id = strdup("t-at");
    turn_add_msg(active, "user", "q2");
    turn_add_msg(active, "assistant", "a2_partial");
    task->active_turn = active;

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *turns_j = cJSON_GetObjectItem(t0, "turns");
    CHECK(cJSON_GetArraySize(turns_j) == 1, "1 committed turn");

    cJSON *at_j = cJSON_GetObjectItem(t0, "active_turn");
    CHECK(at_j != NULL, "active_turn present in JSON");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-at", "ActiveTurn");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt = find_task(session2, "t-at");
    CHECK(rt != NULL, "task restored");

    /* Check committed turns */
    CHECK(edc_queue_length(&rt->turns) == 1, "1 committed turn restored");

    /* Check active turn */
    CHECK(rt->active_turn != NULL, "active_turn restored");
    CHECK(edc_queue_length(&rt->active_turn->messages) == 2,
          "active_turn has 2 messages");

    cJSON *am0 = (cJSON *)rt->active_turn->messages.data[rt->active_turn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(am0, "content")),
                "q2", "active turn msg0 content");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 9: todos, prompt, redirect fields
 *  ══════════════════════════════════════════════════════════════ */

static void test_extra_fields(void)
{
    TEST("todos, prompt, redirect roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-extra", "Extra");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new("t-extra", "sess-extra", "t-extra",
                                          "extra fields", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);

    /* Set redirect */
    task->redirect_stdout = sdsnew("/tmp/out.log");
    task->redirect_stderr = sdsnew("/tmp/err.log");
    task->redirect_append = true;

    /* Set todos */
    task->todos = cJSON_CreateArray();
    cJSON *item1 = cJSON_CreateObject();
    cJSON_AddStringToObject(item1, "content", "review code");
    cJSON_AddStringToObject(item1, "status", "completed");
    cJSON_AddStringToObject(item1, "priority", "low");
    cJSON_AddItemToArray(task->todos, item1);
    cJSON *item2 = cJSON_CreateObject();
    cJSON_AddStringToObject(item2, "content", "write tests");
    cJSON_AddStringToObject(item2, "status", "in_progress");
    cJSON_AddStringToObject(item2, "priority", "high");
    cJSON_AddItemToArray(task->todos, item2);

    /* Set todo sync throttle gap (距上次 todowrite 的 LLM 请求数) */
    task->todo_sync_gap = 7;

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-extra", "Extra");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt = find_task(session2, "t-extra");
    CHECK(rt != NULL, "task restored");

    /* Verify redirect */
    CHECK_STREQ(rt->redirect_stdout, "/tmp/out.log", "redirect_stdout preserved");
    CHECK_STREQ(rt->redirect_stderr, "/tmp/err.log", "redirect_stderr preserved");
    CHECK(rt->redirect_append == true, "redirect_append preserved");

    /* Verify todos */
    CHECK(rt->todos != NULL, "todos restored");
    CHECK(cJSON_GetArraySize(rt->todos) == 2, "todos has 2 items");
    cJSON *ri1 = cJSON_GetArrayItem(rt->todos, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(ri1, "content")),
                "review code", "todos item1 content");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(ri1, "status")),
                "completed", "todos item1 status");

    /* Verify todo sync throttle gap */
    CHECK(rt->todo_sync_gap == 7, "todo_sync_gap preserved");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 10: NULL / empty input safety
 *  ══════════════════════════════════════════════════════════════ */

static void test_null_inputs(void)
{
    TEST("NULL / empty input safety");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-null", "Null");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* serialize with NULL session */
    cJSON *j1 = edc_state_serialize(NULL, NULL);
    CHECK(j1 == NULL, "serialize(NULL, NULL) returns NULL");

    /* serialize no longer depends on session->messages */
    cJSON *j2 = edc_state_serialize(session, NULL);
    CHECK(j2 != NULL, "serialize does not require session->messages");
    cJSON_Delete(j2);

    /* deserialize with NULL json */
    bool ok1 = edc_state_deserialize(NULL, session, NULL);
    CHECK(!ok1, "deserialize(NULL json) returns false");

    /* deserialize with NULL session */
    cJSON *valid = cJSON_Parse("{\"version\":7,\"session\":{},\"groups\":[],\"tasks\":[]}");
    bool ok2 = edc_state_deserialize(valid, NULL, NULL);
    CHECK(!ok2, "deserialize(NULL session) returns false");
    cJSON_Delete(valid);

    /* deserialize with missing version */
    cJSON *no_ver = cJSON_Parse("{\"session\":{},\"groups\":[],\"tasks\":[]}");
    bool ok3 = edc_state_deserialize(no_ver, session, sched);
    CHECK(!ok3, "deserialize without version returns false");
    cJSON_Delete(no_ver);

    /* deserialize with missing tasks array */
    cJSON *no_tasks = cJSON_Parse("{\"version\":7,\"session\":{},\"groups\":[]}");
    bool ok4 = edc_state_deserialize(no_tasks, session, sched);
    CHECK(ok4, "deserialize without tasks array succeeds (treated as empty)");
    cJSON_Delete(no_tasks);

    /* deserialize with tasks not an array */
    cJSON *bad_tasks = cJSON_Parse("{\"version\":7,\"session\":{},\"groups\":[],\"tasks\":\"not_array\"}");
    bool ok5 = edc_state_deserialize(bad_tasks, session, sched);
    CHECK(ok5, "deserialize with non-array tasks succeeds (ignored)");
    cJSON_Delete(bad_tasks);

    /* deserialize with groups not an array */
    cJSON *bad_groups = cJSON_Parse("{\"version\":7,\"session\":{},\"groups\":123,\"tasks\":[]}");
    bool ok6 = edc_state_deserialize(bad_groups, session, sched);
    CHECK(ok6, "deserialize with non-array groups succeeds (ignored)");
    cJSON_Delete(bad_groups);

    edc_scheduler_free(sched);
    edc_session_free(session);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 11: Malformed JSON — bad references
 *  ══════════════════════════════════════════════════════════════ */

static void test_malformed_inputs(void)
{
    TEST("malformed JSON inputs — bad references handled gracefully");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-mal", "Malformed");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* dep referencing nonexistent task */
    const char *bad_dep_json =
        "{"
        "  \"version\":7,"
        "  \"session\":{},"
        "  \"groups\":["
        "    {\"group_id\":\"g1\",\"leader_task_id\":\"t1\","
        "     \"member_task_ids\":[\"t1\"],\"is_orphaned\":false}"
        "  ],"
        "  \"tasks\":["
        "    {\"task_id\":\"t1\",\"group_id\":\"g1\",\"session_id\":\"sess-mal\","
        "     \"description\":\"task 1\",\"state\":\"COMPLETED\","
        "     \"is_group_leader\":true,"
        "     \"fork_point_completion_seq\":0,\"completion_seq\":0,"
        "     \"redirect_append\":false,"
        "     \"deps\":[{\"source_task_id\":\"NOBODY\",\"type\":\"JOIN\","
        "                \"completed\":false,\"cancelled\":false,\"injected\":false}],"
        "     \"turns\":[]}"
        "  ]"
        "}";
    cJSON *j1 = cJSON_Parse(bad_dep_json);
    bool ok1 = edc_state_deserialize(j1, session, sched);
    CHECK(ok1, "deserialize with bad dep reference succeeds (dep ignored)");
    cJSON_Delete(j1);

    /* group member referencing nonexistent task */
    const char *bad_member_json =
        "{"
        "  \"version\":7,"
        "  \"session\":{},"
        "  \"groups\":["
        "    {\"group_id\":\"g1\",\"leader_task_id\":\"t1\","
        "     \"member_task_ids\":[\"t1\",\"NOBODY\"],\"is_orphaned\":false}"
        "  ],"
        "  \"tasks\":["
        "    {\"task_id\":\"t1\",\"group_id\":\"g1\",\"session_id\":\"sess-mal\","
        "     \"description\":\"task 1\",\"state\":\"COMPLETED\","
        "     \"is_group_leader\":true,"
        "     \"fork_point_completion_seq\":0,\"completion_seq\":0,"
        "     \"redirect_append\":false,"
        "     \"deps\":[],\"turns\":[]}"
        "  ]"
        "}";
    cJSON *j2 = cJSON_Parse(bad_member_json);
    bool ok2 = edc_state_deserialize(j2, session, sched);
    CHECK(ok2, "deserialize with bad member reference succeeds (skipped)");
    cJSON_Delete(j2);

    /* turn with no embedded messages (unknown/legacy keys are ignored) */
    const char *bad_range_json =
        "{"
        "  \"version\":7,"
        "  \"session\":{},"
        "  \"groups\":["
        "    {\"group_id\":\"g1\",\"leader_task_id\":\"t1\","
        "     \"member_task_ids\":[\"t1\"],\"is_orphaned\":false}"
        "  ],"
        "  \"tasks\":["
        "    {\"task_id\":\"t1\",\"group_id\":\"g1\",\"session_id\":\"sess-mal\","
        "     \"description\":\"task 1\",\"state\":\"COMPLETED\","
        "     \"is_group_leader\":true,"
        "     \"fork_point_completion_seq\":0,\"completion_seq\":0,"
        "     \"redirect_append\":false,"
        "     \"deps\":[],"
        "     \"turns\":[{\"turn_index\":0,\"message_start\":999,\"message_count\":5}]}"
        "  ]"
        "}";
    cJSON *j3 = cJSON_Parse(bad_range_json);
    bool ok3 = edc_state_deserialize(j3, session, sched);
    CHECK(ok3, "deserialize tolerates a turn with unknown keys (empty turn)");
    cJSON_Delete(j3);

    /* 同一个 task id 被**两个组** claim (手改过的档 / id 撞车的档): 只认第一个。
     * 成员链表用单个 group_next 字段串起来, 一个对象进两个组 = 释放时 double free。 */
    const char *double_claim_json =
        "{"
        "  \"version\":7,"
        "  \"session\":{},"
        "  \"groups\":["
        "    {\"group_id\":\"g1\",\"leader_task_id\":\"t1\",\"member_task_ids\":[\"t1\",\"t2\"],"
        "     \"is_orphaned\":false},"
        "    {\"group_id\":\"g2\",\"leader_task_id\":\"t2\",\"member_task_ids\":[\"t2\"],"
        "     \"is_orphaned\":false}"
        "  ],"
        "  \"tasks\":["
        "    {\"task_id\":\"t1\",\"group_id\":\"g1\",\"session_id\":\"sess-mal\","
        "     \"description\":\"task 1\",\"state\":\"COMPLETED\",\"is_group_leader\":true,"
        "     \"fork_point_completion_seq\":0,\"completion_seq\":0,\"redirect_append\":false,"
        "     \"deps\":[],\"turns\":[]},"
        "    {\"task_id\":\"t2\",\"group_id\":\"g2\",\"session_id\":\"sess-mal\","
        "     \"description\":\"task 2\",\"state\":\"COMPLETED\",\"is_group_leader\":true,"
        "     \"fork_point_completion_seq\":0,\"completion_seq\":0,\"redirect_append\":false,"
        "     \"deps\":[],\"turns\":[]}"
        "  ]"
        "}";
    cJSON *jd = cJSON_Parse(double_claim_json);
    bool okd = edc_state_deserialize(jd, session, sched);
    CHECK(okd, "同一个 task 被两个组 claim: 读回成功 (不崩)");
    {
        /* 数"有几个组的成员表里含 t2" —— 组的运行时 id 来自领队的 task id (不是 JSON 里
         * 那个 group_id 字段), 所以只按成员关系数。 */
        int holders = 0;
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                if (t->task_id && strcmp(t->task_id, "t2") == 0)
                {
                    holders++;
                    break;
                }
            }
        }
        CHECK(holders == 1, "t2 只在一个组里 (不会同时挂在两个组的成员表上)");
    }
    cJSON_Delete(jd);

    /* task with empty task_id */
    const char *empty_id_json =
        "{"
        "  \"version\":7,"
        "  \"session\":{},"
        "  \"groups\":[],"
        "  \"tasks\":["
        "    {\"task_id\":\"\",\"group_id\":\"g1\",\"session_id\":\"s\","
        "     \"description\":\"\",\"state\":\"COMPLETED\","
        "     \"is_group_leader\":false,"
        "     \"fork_point_completion_seq\":0,\"completion_seq\":0,"
        "     \"redirect_append\":false,\"deps\":[],\"turns\":[]}"
        "  ]"
        "}";
    cJSON *j4 = cJSON_Parse(empty_id_json);
    bool ok4 = edc_state_deserialize(j4, session, sched);
    CHECK(ok4, "deserialize with empty task_id succeeds (task skipped)");
    cJSON_Delete(j4);

    edc_scheduler_free(sched);
    edc_session_free(session);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 12: Compact message reference path
 *  ══════════════════════════════════════════════════════════════ */

static void test_compact_messages(void)
{
    TEST("single inline mode: turn embeds its messages");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-cpt", "Compact");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new("t-cpt", "sess-cpt", "t-cpt", "inline test", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);

    /* The turn owns its messages (single inline mode). */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("t-cpt");
    edc_turn_add_user_message(turn, "compact query");
    cJSON *am = cJSON_CreateObject();
    cJSON_AddStringToObject(am, "role", "assistant");
    cJSON_AddStringToObject(am, "content", "compact answer");
    edc_turn_add_assistant_message(turn, am);
    edc_task_add_turn(task, turn);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds");

    cJSON *t0 = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "tasks"), 0);
    cJSON *turn_j = cJSON_GetArrayItem(cJSON_GetObjectItem(t0, "turns"), 0);
    cJSON *embedded = cJSON_GetObjectItem(turn_j, "messages");

    /* v2 单一模式: 一律内联 messages, 不再有 start/count */
    CHECK(embedded != NULL, "single mode: embedded messages array");
    CHECK(cJSON_GetArraySize(embedded) == 2, "embedded 2 messages");
    CHECK(cJSON_GetObjectItem(turn_j, "message_start") == NULL, "single mode: no message_start");
    CHECK(cJSON_GetObjectItem(turn_j, "message_count") == NULL, "single mode: no message_count");

    struct edc_session *session2 = edc_session_new(cfg, "sess-cpt", "Compact");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt = find_task(session2, "t-cpt");
    CHECK(rt != NULL, "task restored");
    CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored");

    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(edc_queue_length(&rturn->messages) == 2, "2 messages restored");

    cJSON *m0 = (cJSON *)rturn->messages.data[rturn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "compact query",
                "msg0 content correct");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 13: Empty turn (0 messages)
 *  ══════════════════════════════════════════════════════════════ */

static void test_empty_turn(void)
{
    TEST("empty turn (0 messages) roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-et", "EmptyTurn");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new("t-et", "sess-et", "t-et",
                                          "empty turn task", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);

    /* Add a turn with 0 messages */
    struct edc_turn *empty_turn = edc_turn_new();
    empty_turn->task_id = strdup("t-et");
    edc_task_add_turn(task, empty_turn);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds");

    /* Verify empty turn JSON */
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *turns_j = cJSON_GetObjectItem(t0, "turns");
    cJSON *turn_j = cJSON_GetArrayItem(turns_j, 0);
    CHECK(turn_j != NULL, "empty turn present in JSON");
    CHECK(cJSON_GetObjectItem(turn_j, "messages") == NULL,
          "empty turn has no messages array");

    struct edc_session *session2 = edc_session_new(cfg, "sess-et", "EmptyTurn");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt = find_task(session2, "t-et");
    CHECK(rt != NULL, "task restored");
    CHECK(edc_queue_length(&rt->turns) == 1, "1 empty turn restored");
    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(edc_queue_length(&rturn->messages) == 0, "turn still has 0 messages");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test 14: Orphaned group roundtrip
 *  ══════════════════════════════════════════════════════════════ */

static void test_orphaned_group(void)
{
    TEST("orphaned group roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-orph", "Orphan");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* Create group with leader + 2 members */
    struct edc_task *leader = edc_task_new("t-leader", "sess-orph", "t-leader",
                                            "group leader", edc_channel_adapter);
    leader->is_group_leader = 1;
    edc_task_mark_ready(leader);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);

    struct edc_task *m1 = edc_task_new("t-m1", "sess-orph", "t-leader",
                                        "member 1", edc_channel_adapter);
    give_input(m1, "member 1 input");

    struct edc_task *m2 = edc_task_new("t-m2", "sess-orph", "t-leader",
                                        "member 2", edc_channel_adapter);
    give_input(m2, "member 2 input");

    struct edc_task_group *group = edc_task_group_new(leader);
    edc_task_group_add_task(group, m1);
    edc_task_group_add_task(group, m2);

    /* Remove leader → group becomes orphaned */
    edc_task_group_remove_task(group, leader);
    /* leader is removed from group but still alive (not freed) */
    /* group is now orphaned, leader=NULL, member_count=2 */

    edc_session_add_group(session, group);
    /* Also add the ex-leader as a separate task for completeness */
    /* (it won't be in any group, but that's fine for this test) */

    CHECK(group->is_orphaned, "group is orphaned");
    CHECK(group->leader == NULL, "leader is NULL");
    CHECK(group->member_count == 2, "2 members remain");

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds");

    /* Verify group JSON */
    cJSON *groups_j = cJSON_GetObjectItem(json, "groups");
    CHECK(cJSON_GetArraySize(groups_j) == 1, "1 group serialized");
    cJSON *gj = cJSON_GetArrayItem(groups_j, 0);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(gj, "is_orphaned")), "is_orphaned=true in JSON");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(gj, "leader_task_id")),
                "t-leader", "leader_task_id preserved as group_id");

    cJSON *members_j = cJSON_GetObjectItem(gj, "member_task_ids");
    CHECK(cJSON_GetArraySize(members_j) == 2, "2 member ids");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-orph", "Orphan");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    CHECK(count_groups(session2) == 1, "1 group restored");
    struct edc_task_group *rg = NULL;
    DL_FOREACH(session2->groups, rg) { break; }
    CHECK(rg != NULL, "group found");
    CHECK(rg->is_orphaned, "group is still orphaned after deserialize");
    CHECK(rg->member_count == 2, "2 members in restored group");

    /* Verify members exist */
    struct edc_task *rm1 = find_task(session2, "t-m1");
    struct edc_task *rm2 = find_task(session2, "t-m2");
    CHECK(rm1 != NULL, "member1 restored");
    CHECK(rm2 != NULL, "member2 restored");
    CHECK(rm1->group == rg, "member1 belongs to group");
    CHECK(rm2->group == rg, "member2 belongs to group");

    cJSON_Delete(json);
    /* Clear turn pointers from session->messages for leader */
    edc_task_free(leader);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  main
 *  ══════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════
 *  Test: fork → child gets independent group + snapshot turn
 *  (mirrors the /task fork command logic)
 *  ══════════════════════════════════════════════════════════════ */

static void test_fork_new_group_snapshot(void)
{
    TEST("fork: child gets independent group with snapshot turn");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-forkgrp", "ForkGrp");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* parent group with history: [system, user1, assistant1] */
    struct edc_task *parent = edc_task_new("task-parent", "sess-forkgrp", "task-parent",
                                            "parent task", edc_channel_adapter);
    edc_task_mark_ready(parent);
    edc_task_await_exec(parent);
    edc_task_mark_completed(parent);
    struct edc_task_group *pg = edc_scheduler_create_group(sched, session, parent);

    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", "You are EdgeClaw.");
    cJSON_AddItemToArray(pg->history, sys);

    struct edc_turn *pt = edc_turn_new();
    pt->task_id = strdup("task-parent");
    turn_add_msg(pt, "user", "hello");
    turn_add_msg(pt, "assistant", "hi there");
    edc_task_add_turn(parent, pt);
    for (size_t m = 0; m < edc_queue_length(&pt->messages); m++)
        cJSON_AddItemReferenceToArray(pg->history, pt->messages.data[m]); /* STOP sync */

    /* ── fork (mirror edc_commands_handle_task_fork) ── */
    size_t fork_point = (size_t)cJSON_GetArraySize(pg->history);
    struct edc_task *child = edc_task_fork(parent, "task-child", "child", fork_point, edc_channel_adapter);
    struct edc_task_group *cg = edc_scheduler_create_group(sched, session, child);

    if (pg->memory)
        cg->memory = sdsdup(pg->memory);
    cJSON *psys = cJSON_GetArrayItem(pg->history, 0);
    if (psys)
        cJSON_AddItemToArray(cg->history, cJSON_Duplicate(psys, 1));

    size_t snap_begin = 1; /* 已折消息不在 history 里 (折叠落地即剪枝) */
    struct edc_turn *snap = edc_turn_new();
    snap->task_id = strdup(child->task_id);
    for (size_t i = snap_begin; i < fork_point; i++)
    {
        cJSON *src = cJSON_GetArrayItem(pg->history, i);
        if (!src)
            break;
        cJSON *msg = cJSON_Duplicate(src, 1);
        edc_queue_push(&snap->messages, msg);
        cJSON_AddItemReferenceToArray(cg->history, msg);
    }
    if (edc_queue_length(&snap->messages) > 0)
        edc_task_add_turn(child, snap);
    else
        edc_turn_free(snap);

    /* 新模型: /task fork 同时把 desc 当 input 装到子任务上 (子任务创建时就带着它的
     * input); 这里照做, 否则这条只有历史、没有待跑 turn 的子任务会在落盘时被丢掉。 */
    {
        struct edc_turn *cturn = edc_turn_new();
        cturn->task_id = strdup(child->task_id);
        edc_turn_add_user_message(cturn, "child input");
        child->active_turn = cturn;
        edc_task_mark_interrupted(child);
    }

    /* ── runtime invariants ── */
    CHECK(child->group == cg, "child in own group");
    CHECK(child->group != pg, "child not in parent group");
    CHECK(child->is_group_leader == 1, "child is group leader");
    CHECK_STREQ(child->group_id, "task-child", "child group_id is its own id");
    CHECK(cJSON_GetArraySize(cg->history) == 3, "child group history = system + 2 snapshot msgs");
    CHECK(cJSON_GetArraySize(pg->history) == 3, "parent group history unchanged");
    cJSON *csys = cJSON_GetArrayItem(cg->history, 0);
    CHECK(csys && cJSON_GetObjectItem(csys, "role") &&
              strcmp(cJSON_GetObjectItem(csys, "role")->valuestring, "system") == 0,
          "child history[0] is system");
    CHECK(edc_queue_length(&child->turns) == 1, "child has 1 synthetic turn");
    struct edc_turn *c0 = child->turns.data[child->turns.head];
    CHECK(edc_queue_length(&c0->messages) == 2, "snapshot turn has 2 messages");

    /* ── serialize + deserialize ── */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize ok");
    struct edc_session *session2 = edc_session_new(cfg, "sess-forkgrp", "ForkGrp");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    CHECK(edc_state_deserialize(json, session2, sched2), "deserialize ok");

    CHECK(count_groups(session2) == 2, "2 groups after roundtrip");
    struct edc_task *rp = find_task(session2, "task-parent");
    struct edc_task *rc = find_task(session2, "task-child");
    CHECK(rp != NULL && rc != NULL, "parent and child restored");
    if (rp && rc)
    {
        CHECK(rc->group != NULL && rp->group != NULL, "both have groups");
        CHECK(rc->group != rp->group, "child group distinct from parent group");
        CHECK_STREQ(rc->group->group_id, "task-child", "child restored in own group");
        CHECK(rc->forked_from == rp, "forked_from preserved");
        CHECK(rc->group->history != NULL, "child group history rebuilt");
        CHECK(cJSON_GetArraySize(rc->group->history) == 3, "child group history rebuilt to 3 msgs");
    }

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  Test: fork snapshot retrieves folded parent turns (rule 2)
 *  ══════════════════════════════════════════════════════════════ */

static void test_fork_snapshot_turns_flushed(void)
{
    TEST("edc_task_snapshot_turns retrieves committed turns (rule 2)");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-snapfl", "SnapFlush");

    /* 建 task + turn (P4: 消息留在 turn, 不再 move 到 session) */
    struct edc_task *task = edc_task_new("task-parent", "sess-snapfl", "task-parent",
                                          "parent", edc_channel_adapter);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-parent");
    turn_add_msg(turn, "user", "folded query");
    turn_add_msg(turn, "assistant", "folded answer");
    edc_task_add_turn(task, turn); /* commit 到 task->turns */

    CHECK(edc_queue_length(&turn->messages) == 2, "turn retains messages");

    /* 模拟 L1 折叠 */
    turn->folded_prefix = (int)edc_queue_length(&turn->messages); /* 模拟 L1 折叠 */

    /* 规则 2 捞回 */
    struct edc_turn *snap = edc_task_snapshot_turns(task, session, "task-child");
    CHECK(snap != NULL, "snapshot non-null");
    if (snap)
    {
        CHECK(edc_queue_length(&snap->messages) == 2, "snapshot has 2 msgs");
        CHECK_STREQ(snap->task_id, "task-child", "snapshot task_id is child");
        cJSON *m0 = (cJSON *)snap->messages.data[snap->messages.head];
        cJSON *m1 = (cJSON *)snap->messages.data[snap->messages.head + 1];
        CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "role")), "user", "msg0 role user");
        CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m1, "content")), "folded answer", "msg1 content");
        edc_turn_free(snap);
    }

    edc_turn_free(turn);
    edc_session_free(session);
    edc_config_free(cfg);
}

static void test_fork_snapshot_turns_unflushed(void)
{
    TEST("edc_task_snapshot_turns copies unflushed turns");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-snapun", "SnapUnfl");

    struct edc_task *task = edc_task_new("task-parent", "sess-snapun", "task-parent",
                                          "parent", edc_channel_adapter);
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-parent");
    turn_add_msg(turn, "user", "unflushed q");
    turn_add_msg(turn, "assistant", "unflushed a");
    edc_task_add_turn(task, turn); /* 未 flush */

    struct edc_turn *snap = edc_task_snapshot_turns(task, session, "task-child");
    CHECK(snap != NULL, "snapshot non-null");
    if (snap)
    {
        CHECK(edc_queue_length(&snap->messages) == 2, "snapshot has 2 msgs");
        cJSON *m1 = (cJSON *)snap->messages.data[snap->messages.head + 1];
        CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m1, "content")), "unflushed a", "msg1 content");
        edc_turn_free(snap);
    }

    edc_turn_free(turn);
    edc_session_free(session);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  v3: message provenance (ts + render) persists across a roundtrip
 *  ══════════════════════════════════════════════════════════════ */

static void test_message_provenance(void)
{
    TEST("message ts/render persist across roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-prov", "Prov");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new("tp", "sess-prov", "tp", "d", edc_channel_adapter);
    /* 落在 READY 的 task 必须带着 input 才落盘 (turn-less 的 READY 是残渣)。 */
    give_input(task, "tp active input");
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("tp");
    /* Real creation path: edc_turn_add_user_message stamps ts + render. */
    edc_turn_add_user_message(turn, "hi");
    edc_task_add_turn(task, turn);
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize");

    cJSON *t0 = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "tasks"), 0);
    CHECK(t0 != NULL, "task serialized");
    cJSON *turn0 = t0 ? cJSON_GetArrayItem(cJSON_GetObjectItem(t0, "turns"), 0) : NULL;
    cJSON *m0 = turn0 ? cJSON_GetArrayItem(cJSON_GetObjectItem(turn0, "messages"), 0) : NULL;
    CHECK(m0 != NULL, "message serialized");
    CHECK(m0 && cJSON_GetObjectItem(m0, "ts") != NULL, "ts serialized");
    CHECK(m0 && cJSON_IsString(cJSON_GetObjectItem(m0, "ts")), "ts is a string");
    CHECK_STREQ(m0 ? cJSON_GetStringValue(cJSON_GetObjectItem(m0, "render")) : NULL, "plain",
                "render serialized (default plain)");

    struct edc_session *s2 = edc_session_new(cfg, "sess-prov", "Prov");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_deserialize(json, s2, sched2), "deserialize");

    struct edc_task *rt = find_task(s2, "tp");
    CHECK(rt != NULL, "task restored");
    struct edc_turn *rturn = rt ? (struct edc_turn *)rt->turns.data[rt->turns.head] : NULL;
    cJSON *rm = rturn ? (cJSON *)rturn->messages.data[rturn->messages.head] : NULL;
    CHECK(rm && cJSON_GetObjectItem(rm, "ts") != NULL, "ts roundtrips");
    CHECK_STREQ(rm ? cJSON_GetStringValue(cJSON_GetObjectItem(rm, "render")) : NULL, "plain", "render roundtrips");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(s2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  v3: fold watermark (completion_seq) — serialize + v2 derivation
 *  ══════════════════════════════════════════════════════════════ */

static void test_fold_watermark(void)
{
    TEST("fold watermark (completion_seq) roundtrip + v2 derivation");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-fold", "Fold");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    /* t1 (completion_seq=1) folded; t2 (completion_seq=2) not folded. */
    struct edc_task *t1 = edc_task_new("t1", "sess-fold", "t1", "one", edc_channel_adapter);
    edc_task_await_exec(t1);
    edc_task_mark_completed(t1);
    t1->completion_seq = 1;
    struct edc_turn *turn1 = edc_turn_new();
    turn1->task_id = strdup("t1");
    turn_add_msg(turn1, "user", "a");
    turn_add_msg(turn1, "assistant", "b");
    edc_task_add_turn(t1, turn1);
    turn1->folded_prefix = 2; /* 模拟 L1 折叠: 整条进了记忆 */

    struct edc_task_group *group = edc_task_group_new(t1);
    edc_session_add_group(session, group);

    struct edc_task *t2 = edc_task_new("t2", "sess-fold", "t1", "two", edc_channel_adapter);
    edc_task_await_exec(t2);
    edc_task_mark_completed(t2);
    t2->completion_seq = 2;
    struct edc_turn *turn2 = edc_turn_new();
    turn2->task_id = strdup("t2");
    turn_add_msg(turn2, "user", "c");
    edc_task_add_turn(t2, turn2);
    edc_task_group_add_task(group, t2);

    session->next_completion_seq = 3;
    /* 折叠 epoch 与锚的位置信息一起来回: 组 ctx 的可用性判断全依赖它们。 */
    group->fold_epoch = 7;
    t1->last_total_tokens = 1200;
    t1->last_fold_epoch = 7;
    t1->last_hist_len = 2;
    t1->last_turn_len = 1;

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize");
    cJSON *gj = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "groups"), 0);
    CHECK(gj != NULL, "group serialized");
    cJSON *fe = gj ? cJSON_GetObjectItem(gj, "fold_epoch") : NULL;
    CHECK(fe && cJSON_GetNumberValue(fe) == 7, "fold epoch serialized");
    {
        cJSON *tj = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "tasks"), 0);
        cJSON *lt = tj ? cJSON_GetObjectItem(tj, "last_total_tokens") : NULL;
        cJSON *lh = tj ? cJSON_GetObjectItem(tj, "last_hist_len") : NULL;
        cJSON *ltu = tj ? cJSON_GetObjectItem(tj, "last_turn_len") : NULL;
        cJSON *le = tj ? cJSON_GetObjectItem(tj, "last_fold_epoch") : NULL;
        CHECK(lt && cJSON_GetNumberValue(lt) == 1200, "anchor size serialized");
        CHECK(lh && cJSON_GetNumberValue(lh) == 2, "anchor history position serialized");
        CHECK(ltu && cJSON_GetNumberValue(ltu) == 1, "anchor turn position serialized");
        CHECK(le && cJSON_GetNumberValue(le) == 7, "anchor fold epoch serialized");
    }

    /* (a) v3 file: watermark read back verbatim. */
    struct edc_session *s2 = edc_session_new(cfg, "sess-fold", "Fold");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_deserialize(json, s2, sched2), "deserialize v3");
    struct edc_task_group *g2 = find_group(s2, "t1");
    CHECK(g2 != NULL, "group restored");
    if (g2)
    {
        {
            struct edc_task *rt = find_task(s2, "t1");
            struct edc_turn *rturn = (rt && rt->turns.head < rt->turns.tail)
                                         ? (struct edc_turn *)rt->turns.data[rt->turns.head]
                                         : NULL;
            CHECK(rturn && rturn->folded_prefix == 2, "the turn's folded prefix survives the round trip");
        }
        CHECK(g2->fold_epoch == 7, "fold epoch preserved");
        {
            struct edc_task *rt = find_task(s2, "t1");
            CHECK(rt && rt->last_total_tokens == 1200 && rt->last_hist_len == 2 && rt->last_turn_len == 1 &&
                      rt->last_fold_epoch == 7,
                  "the context anchor (size + position + epoch) survives the round trip");
        }
        CHECK(cJSON_GetArraySize(g2->history) == 2, "已折的 2 条不在 history 里 (sys + 剩 1 条)");
        CHECK(edc_task_group_unfolded_msg_count(g2) == 1, "unfolded = 1 msg");
    }

    /* (b) a turn that predates the folded_prefix key: it reads as 0 — nothing
     * folded, nothing guessed. Filling it in is the v5->v6 migration's job; the
     * loader never derives folded-ness from anywhere else. */
    cJSON *tasks_arr = cJSON_GetObjectItem(json, "tasks");
    for (size_t i = 0; tasks_arr && i < (size_t)cJSON_GetArraySize(tasks_arr); i++)
    {
        cJSON *tjx = cJSON_GetArrayItem(tasks_arr, i);
        cJSON *turns = tjx ? cJSON_GetObjectItem(tjx, "turns") : NULL;
        for (size_t k = 0; turns && k < (size_t)cJSON_GetArraySize(turns); k++)
        {
            cJSON *turnx = cJSON_GetArrayItem(turns, k);
            if (turnx)
                cJSON_DeleteItemFromObject(turnx, "folded_prefix");
        }
    }
    struct edc_session *s3 = edc_session_new(cfg, "sess-fold", "Fold");
    struct edc_scheduler *sched3 = edc_scheduler_new(cfg);
    install_resolver(sched3, s3);
    CHECK(edc_state_deserialize(json, s3, sched3), "deserialize without a watermark key");
    struct edc_task_group *g3 = find_group(s3, "t1");
    CHECK(g3 != NULL, "group restored (no watermark key)");
    if (g3)
    {
        CHECK(edc_task_group_unfolded_msg_count(g3) == 3, "no key -> nothing folded, all 3 msgs unfolded");
        CHECK(cJSON_GetArraySize(g3->history) == 4, "…and the history still carries them (sys + 3)");
    }

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(s2);
    edc_scheduler_free(sched2);
    edc_session_free(s3);
    edc_scheduler_free(sched3);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  schema version: only the current one is accepted
 *  ══════════════════════════════════════════════════════════════ */

static void test_schema_version_is_current(void)
{
    TEST("serialize writes the current version; older versions are still accepted, older/newer fails loudly");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-ver", "Ver");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new("1", "sess-ver", "1", "task", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_mark_interrupted(task);
    struct edc_task_group *grp = edc_task_group_new(task);
    grp->memory = sdsnew("# remembered");
    grp->memory_created_at = 111;
    grp->memory_updated_at = 222;
    grp->memory_covers_to = 333;
    edc_session_add_group(session, grp);

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");
    /* 本文件构造 JSON 时用的版本号是手写镜像 (见各 cJSON_Parse 字面量): 一律等于
     * 当前 schema, 升级时这里会先红。 */
    CHECK(edc_state_schema_version() == 12, "schema version bumped -> update this file's literals");
    CHECK(edc_state_json_version(json) == edc_state_schema_version(), "on-disk version is the current schema");

    struct edc_session *s2 = edc_session_new(cfg, "sess-ver", "Ver");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_deserialize(json, s2, sched2), "the current schema round-trips");
    CHECK(edc_session_find_task(s2, "1") != NULL, "task restored");
    {
        struct edc_task_group *rg = edc_session_find_group(s2, "1");
        CHECK(rg && rg->memory_created_at == 111 && rg->memory_updated_at == 222 && rg->memory_covers_to == 333,
              "group memory times round-trip (v9)");
    }
    edc_session_free(s2);
    edc_scheduler_free(sched2);

    /* The immediately preceding schema (v7) must still load: a bump must not strand
     * the archives the previous binary wrote. Fields v7 lacks read back as defaults. */
    cJSON_ReplaceItemInObject(json, "version", cJSON_CreateNumber(7));
    struct edc_session *s7 = edc_session_new(cfg, "sess-ver", "Ver");
    struct edc_scheduler *sched7 = edc_scheduler_new(cfg);
    install_resolver(sched7, s7);
    CHECK(edc_state_deserialize(json, s7, sched7), "a v7 archive is still accepted");
    edc_session_free(s7);
    edc_scheduler_free(sched7);

    /* A pre-migration file must be rejected: the load path no longer carries the
     * v3 compatibility branches (system-container drop, displayed/header strip)
     * -- those moved into tools/migrate_session_v3_to_v4.py. */
    cJSON_ReplaceItemInObject(json, "version", cJSON_CreateNumber(3));
    struct edc_session *s3 = edc_session_new(cfg, "sess-ver", "Ver");
    struct edc_scheduler *sched3 = edc_scheduler_new(cfg);
    install_resolver(sched3, s3);
    CHECK(!edc_state_deserialize(json, s3, sched3), "a v3 tree is rejected");
    edc_session_free(s3);
    edc_scheduler_free(sched3);

    /* A version NEWER than this binary also refuses (do not guess a future shape). */
    cJSON_ReplaceItemInObject(json, "version", cJSON_CreateNumber(13));
    struct edc_session *s9 = edc_session_new(cfg, "sess-ver", "Ver");
    struct edc_scheduler *sched9 = edc_scheduler_new(cfg);
    install_resolver(sched9, s9);
    CHECK(!edc_state_deserialize(json, s9, sched9), "a future version is rejected");
    edc_session_free(s9);
    edc_scheduler_free(sched9);

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  message unknown keys round-trip verbatim
 *  ══════════════════════════════════════════════════════════════ */

static void test_message_unknown_keys_round_trip(void)
{
    TEST("message unknown keys round-trip verbatim (the app no longer strips them)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-disp", "Disp");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new("1", "sess-disp", "1", "task", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_mark_interrupted(task);
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    struct edc_turn *turn = edc_turn_new();
    edc_turn_add_user_message(turn, "hi");
    task->active_turn = turn;

    cJSON *json = edc_state_serialize(session, sched);

    /* Splice the 5c keys into the stored message, the way an old file has them. */
    cJSON *json_task = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "tasks"), 0);
    cJSON *json_msgs = cJSON_GetObjectItem(cJSON_GetObjectItem(json_task, "active_turn"), "messages");
    cJSON *json_msg = cJSON_GetArrayItem(json_msgs, 0);
    CHECK(json_msg != NULL, "message found in the serialized turn");
    cJSON_AddBoolToObject(json_msg, "displayed", true);
    cJSON_AddStringToObject(json_msg, "header", "exec_shell");

    struct edc_session *s2 = edc_session_new(cfg, "sess-disp", "Disp");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_deserialize(json, s2, sched2), "deserialize");

    struct edc_task *t2 = edc_session_find_task(s2, "1");
    CHECK(t2 && t2->active_turn, "turn restored");
    CHECK(t2 && t2->active_turn && cJSON_GetObjectItem((cJSON *)t2->active_turn->messages.data[0], "displayed") != NULL,
          "messages keep unknown keys in memory (raw JSON, not rebuilt field by field)");

    cJSON *json2 = edc_state_serialize(s2, sched2);
    cJSON *t0 = cJSON_GetArrayItem(cJSON_GetObjectItem(json2, "tasks"), 0);
    cJSON *msgs2 = cJSON_GetObjectItem(cJSON_GetObjectItem(t0, "active_turn"), "messages");
    cJSON *msg2 = cJSON_GetArrayItem(msgs2, 0);
    CHECK(msg2 != NULL, "message written back");
    /* Raw JSON round-trip: keys the app does not know are preserved.  Removing
     * pre-transcript leftovers is the one-shot migration's job, not save's. */
    CHECK(msg2 && cJSON_GetObjectItem(msg2, "displayed") != NULL, "displayed round-trips");
    CHECK(msg2 && cJSON_GetObjectItem(msg2, "header") != NULL, "header round-trips");
    CHECK(msg2 && cJSON_GetObjectItem(msg2, "role") != NULL, "role kept");
    CHECK(msg2 && cJSON_GetObjectItem(msg2, "ts") != NULL, "ts kept");

    cJSON_Delete(json2);
    cJSON_Delete(json);
    edc_session_free(s2);
    edc_scheduler_free(sched2);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ══════════════════════════════════════════════════════════════
 *  continuation: 相位必须在往返中**幸存**
 *  ══════════════════════════════════════════════════════════════ */

/* 这条是 P2 的第一块砖, 而且它**今天就是红的** —— 那正是它存在的意义。
 *
 * 现状 (runtime-model.md §1.4, 台账第 4 条): 相位**存得下来** —— 上面
 * test_state_degradation 自己就断言了 JSON 里写着 "LLM_PROCESSING"/"TOOL_CALLING"
 * —— 但**读不回**: normalize_interrupted_state 把五个相位一起塌成 INTERRUPTED,
 * resume 再靠顺序探测 (deps -> 工具账本 -> active_turn) 把相位猜回来。
 *
 * 丢的不只是"叫什么名字": 一个正在等工具结果的 task, 和一个刚建好还没跑的
 * task, 落盘后变成同一个状态。resume 只能靠残留物反推; 推不出来就退化到
 * WAITING_INPUT —— "任务到哪了"散在多处、且会丢, 就是这么来的。
 *
 * P2 之后: 相位原样回来, 下面的断言全绿; 而 test_state_degradation 要改成
 * 表达新契约 (它现在钉的是旧行为, 这正是"行为变更先补测试"要面对的东西)。
 *
 * 变异测试: 把 step 再粗化回 INTERRUPTED, 这条必须**红**。 */
static void test_continuation_preserved(void)
{
    TEST("interrupted phases survive the round trip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-cont", "Continuation");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *t_llm = edc_task_new("t-llm", "sess-cont", "t-llm", "llm processing", edc_channel_adapter);
    t_llm->is_group_leader = 1;
    edc_task_mark_ready(t_llm);
    edc_task_await_exec(t_llm);

    struct edc_task *t_tool = edc_task_new("t-tool", "sess-cont", "t-llm", "tool calling", edc_channel_adapter);
    edc_task_await_exec(t_tool);
    edc_task_await_tools(t_tool);

    struct edc_task *t_input = edc_task_new("t-input", "sess-cont", "t-llm", "waiting input", edc_channel_adapter);
    /* READY 的 task 也必须带着 input 才落盘, 否则它和 "刚建好还没跑" 无处区分;
     * 没有 turn 的 READY 任务会被落盘判据丢掉 (docs/retire-waiting-input.md §4)。 */
    give_input(t_input, "input for t-input");

    struct edc_task_group *group = edc_task_group_new(t_llm);
    edc_task_group_add_task(group, t_tool);
    edc_task_group_add_task(group, t_input);
    edc_session_add_group(session, group);

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    struct edc_session *session2 = edc_session_new(cfg, "sess-cont", "Continuation");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *r_llm = find_task(session2, "t-llm");
    struct edc_task *r_tool = find_task(session2, "t-tool");
    struct edc_task *r_input = find_task(session2, "t-input");

    CHECK(r_llm && r_llm->step == EDC_TASKSTEP_AWAIT_EXEC, "step AWAIT_EXEC survives");
    CHECK(r_tool && r_tool->step == EDC_TASKSTEP_AWAIT_TOOLS, "step AWAIT_TOOLS survives");
    /* t-input 停在 READY: 旧名 AWAIT_INPUT 已经退休 —— "等用户输入"不是相位,
     * task 创建时就带着它的 input。载入时旧文件里的 "AWAIT_INPUT" 也折成 READY。 */
    CHECK(r_input && r_input->step == EDC_TASKSTEP_READY, "step READY survives");

    /* 活着吗统一收敛 (重启后没有任何东西在跑) —— 但那是**另一件事**, 不该把 step 带走。 */
    CHECK(r_llm && r_llm->life == EDC_LIFE_INTERRUPTED, "the in-flight one is INTERRUPTED");

    /* 而且三个必须**互不相同** —— 否则"存下来"没有意义。 */
    CHECK(r_llm && r_tool && r_input && r_llm->step != r_tool->step && r_tool->step != r_input->step,
          "the three steps are still distinct");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}


/* ── `||` (UNLESS) 的存档往返 ────────────────────────────────────────────
 * 两类"放行型"边的名字必须落盘成字符串且能读回（`dep_type_from_name` 缺一条就会**静默降级
 * 成 JOIN**）。同时钉住加载时的镜像：源已完成 ⇒ 后件被跳过，即使文件里那条边还是 outstanding。 */
static void test_unless_dep_roundtrip(void)
{
    TEST("|| (UNLESS) dep roundtrip: name survives, and a completed source skips the dependent");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-unless", "Unless");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *ta = edc_task_new("task-OK", "sess-unless", "task-OK", "the one that works",
                                       edc_channel_adapter);
    ta->is_group_leader = 1;
    edc_task_mark_ready(ta);
    edc_task_await_exec(ta);
    edc_task_mark_completed(ta);
    ta->output = sdsnew("fine");

    struct edc_task *tb = edc_task_new("task-FALLBACK", "sess-unless", "task-OK", "the fallback",
                                       edc_channel_adapter);
    edc_task_mark_waiting_deps(tb);
    {
        /* 带 turn 才会落盘（README 判据：无 turn 的 READY 任务被丢弃）。 */
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(tb->task_id);
        edc_turn_add_user_message(turn, "fallback input");
        tb->active_turn = turn;
    }
    CHECK(edc_task_add_dependency(tb, ta, TASK_DEP_UNLESS), "tb -UNLESS-> ta");

    struct edc_task_group *group = edc_task_group_new(ta);
    edc_task_group_add_task(group, tb);
    edc_session_add_group(session, group);

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    struct edc_session *session2 = edc_session_new(cfg, "sess-unless", "Unless");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    CHECK(edc_state_deserialize(json, session2, sched2), "deserialize succeeds");

    struct edc_task *rb = find_task(session2, "task-FALLBACK");
    CHECK(rb != NULL, "the dependent came back");
    if (rb)
    {
        CHECK(rb->deps != NULL, "with its edge");
        CHECK(rb->deps && rb->deps->type == TASK_DEP_UNLESS,
              "and the type is UNLESS (a missing name in dep_type_from_name would make it JOIN)");
        CHECK(edc_task_is_cancelled(rb), "the source succeeded, so the `||` branch was skipped");
    }

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* v10: the reason a task ended must survive the save, and an older file (no `end`) must
 * map conservatively -- never to BLOCKED, because that is the value a future auto-release
 * would act on. Backed by the two facts this axis exists for: CANCELLED covers "the user
 * stopped it" / "a `||` branch was skipped" / "a dependency took it down", and ERROR
 * covers "it failed" / "a dependency never produced". */
static void test_task_end_roundtrip(void)
{
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-end", "Ends");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *leader = edc_task_new("end-leader", "sess-end", "end-leader", "leader", edc_channel_adapter);
    /* Mid-request: the persist predicate drops a task that sits READY+ALIVE with nothing
     * in flight (its committed turns do not count), and a group whose leader is dropped is
     * restored as nothing at all. */
    edc_task_await_exec(leader);
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_session_add_group(session, group);

    struct edc_task *source = edc_task_new("end-src", "sess-end", "end-leader", "source", edc_channel_adapter);
    edc_task_group_add_task(group, source);
    struct edc_task *blocked = edc_task_new("end-blocked", "sess-end", "end-leader", "blocked", edc_channel_adapter);
    edc_task_group_add_task(group, blocked);
    struct edc_task *stopped = edc_task_new("end-stopped", "sess-end", "end-leader", "stopped", edc_channel_adapter);
    edc_task_group_add_task(group, stopped);
    struct edc_task *running = edc_task_new("end-running", "sess-end", "end-leader", "running", edc_channel_adapter);
    edc_task_group_add_task(group, running);
    edc_task_await_exec(running); /* mid-request: the state a restart interrupts */
    struct edc_task *failed = edc_task_new("end-failed", "sess-end", "end-leader", "failed", edc_channel_adapter);
    edc_task_group_add_task(group, failed);

    /* A dependency that is interrupted takes its dependent down: BLOCKED, not FAILED. */
    edc_task_add_dependency(blocked, source, TASK_DEP_SEQUENCE);
    edc_task_await_exec(source);
    edc_task_mark_interrupted(source);
    /* The user's own stop: STOPPED, not SKIPPED. */
    edc_task_send_signal(stopped, TASK_SIGTERM);
    /* A failure whose cause is knowable: the class must survive the save, or a resume
     * cannot warn "this will fail the same way". */
    edc_task_await_exec(failed);
    edc_task_mark_failed(failed);
    edc_task_set_fail(failed, EDC_FAIL_AUTH);

    TEST("end: reason / blocker / the source's own reason are written");
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(json, "groups")) == 1, "the group is written");
    CHECK(cJSON_GetArraySize(tasks_j) == 6, "six tasks are written");
    for (int i = 0; i < cJSON_GetArraySize(tasks_j); i++)
    {
        cJSON *t = cJSON_GetArrayItem(tasks_j, i);
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(t, "task_id"));
        if (!id)
            continue;
        const char *end = cJSON_GetStringValue(cJSON_GetObjectItem(t, "end"));
        if (strcmp(id, "end-src") == 0)
            CHECK_STREQ(end, "INTERRUPTED", "the interrupted source says INTERRUPTED");
        else if (strcmp(id, "end-blocked") == 0)
        {
            CHECK_STREQ(end, "BLOCKED", "the cascaded dependent says BLOCKED");
            CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t, "blocked_by")), "end-src", "the blocker is named");
            CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t, "blocked_end")), "INTERRUPTED",
                        "and what became of it");
        }
        else if (strcmp(id, "end-stopped") == 0)
            CHECK_STREQ(end, "STOPPED", "the user's stop says STOPPED");
        else if (strcmp(id, "end-running") == 0)
            CHECK_STREQ(end, "NONE", "a task that was still running has no reason");
        else if (strcmp(id, "end-failed") == 0)
        {
            CHECK_STREQ(end, "FAILED", "its own failure says FAILED");
            CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t, "fail")), "AUTH", "and the failure class rides along");
        }
    }

    TEST("end: the reasons survive a roundtrip, and a running task lands as INTERRUPTED");
    struct edc_session *session2 = edc_session_new(cfg, "sess-end", "Ends");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);
    CHECK(edc_state_deserialize(json, session2, sched2), "deserialize succeeds");

    struct edc_task *r_src = find_task(session2, "end-src");
    struct edc_task *r_blocked = find_task(session2, "end-blocked");
    struct edc_task *r_stopped = find_task(session2, "end-stopped");
    struct edc_task *r_running = find_task(session2, "end-running");
    CHECK(r_src && r_src->end == EDC_END_INTERRUPTED, "INTERRUPTED preserved");
    CHECK(r_blocked && r_blocked->end == EDC_END_BLOCKED, "BLOCKED preserved");
    CHECK(r_blocked && r_blocked->blocked_by && strcmp(r_blocked->blocked_by, "end-src") == 0,
          "the blocker id is preserved");
    CHECK(r_blocked && r_blocked->blocked_end == EDC_END_INTERRUPTED, "and the source's reason");
    CHECK(r_stopped && r_stopped->end == EDC_END_STOPPED, "STOPPED preserved");
    CHECK(r_running && r_running->life == EDC_LIFE_INTERRUPTED, "a running task reloads as INTERRUPTED");
    CHECK(r_running && r_running->end == EDC_END_INTERRUPTED, "and says why");
    struct edc_task *r_failed = find_task(session2, "end-failed");
    CHECK(r_failed && r_failed->end == EDC_END_FAILED, "FAILED preserved");
    CHECK(r_failed && r_failed->fail == EDC_FAIL_AUTH, "and its failure class (v11)");

    TEST("end: an older file without `end` maps conservatively (never BLOCKED)");
    /* Same archive, v9: the reason keys are gone, exactly as an older binary wrote them. */
    cJSON_ReplaceItemInObject(json, "version", cJSON_CreateNumber(9));
    for (int i = 0; i < cJSON_GetArraySize(tasks_j); i++)
    {
        cJSON *t = cJSON_GetArrayItem(tasks_j, i);
        cJSON_DeleteItemFromObject(t, "end");
        cJSON_DeleteItemFromObject(t, "blocked_by");
        cJSON_DeleteItemFromObject(t, "blocked_end");
        cJSON_DeleteItemFromObject(t, "fail"); /* v10 and older never wrote it */
    }
    struct edc_session *session3 = edc_session_new(cfg, "sess-end", "Ends");
    struct edc_scheduler *sched3 = edc_scheduler_new(cfg);
    install_resolver(sched3, session3);
    CHECK(edc_state_deserialize(json, session3, sched3), "v9 deserialize succeeds");

    struct edc_task *o_blocked = find_task(session3, "end-blocked");
    struct edc_task *o_stopped = find_task(session3, "end-stopped");
    CHECK(o_blocked && o_blocked->end == EDC_END_FAILED,
          "an ERROR row with no reason is FAILED -- never BLOCKED, so nothing is auto-released on a guess");
    CHECK(o_blocked && o_blocked->blocked_by == NULL, "and no blocker is invented");
    CHECK(o_stopped && o_stopped->end == EDC_END_STOPPED, "a CANCELLED row reads back as STOPPED");
    struct edc_task *o_failed = find_task(session3, "end-failed");
    CHECK(o_failed && o_failed->fail == EDC_FAIL_NONE,
          "no failure class in an old archive -> NONE (a resume cannot claim to know)");

    cJSON_Delete(json);
    /* The fixtures are owned by their sessions here (freed with the process), as in the
     * rest of this file. */
}

int main(void)
{
    log_info("=== Phase 1: edc_state_serializer module tests ===");
    log_info("");

    test_empty_session();
    test_single_task();
    test_fork_chain();
    test_fork_new_group_snapshot();
    test_fork_snapshot_turns_flushed();
    test_fork_snapshot_turns_unflushed();
    test_dependency_graph();
    test_unless_dep_roundtrip();
    test_complex_topology();
    test_turnless_shell_is_dropped();
    test_state_degradation();
    test_continuation_preserved();
    test_schema_version_is_current();
    test_message_unknown_keys_round_trip();
    test_filter_transient_states();
    test_cancelled_midflight_roundtrip();
    test_cancelled_dep_roundtrip();
    test_join_cascade_roundtrip();
    test_active_turn();
    test_extra_fields();
    test_null_inputs();
    test_malformed_inputs();
    test_compact_messages();
    test_empty_turn();
    test_orphaned_group();
    test_message_provenance();
    test_fold_watermark();
    test_task_end_roundtrip();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
