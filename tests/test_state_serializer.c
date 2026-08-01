#include "src/edc_state_serializer.h"
#include "src/edc_session.h"
#include "src/edc_task.h"
#include "src/edc_task_group.h"
#include "src/edc_turn.h"
#include "src/edc_scheduler.h"
#include "src/edc_channel_actor.h"
#include "src/edc_config.h"
#include "src/edc_compat.h"
#include "src/utlist_fix.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>

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
    /* add a dummy timestamp too */
    edc_queue_push(&turn->timestamps, sdsnew("2026-01-01T00:00:00"));
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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

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
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* Build a task with one completed turn */
    struct edc_channel *ch = edc_channel_cli_new();
    struct edc_task *task = edc_task_new("task-1", "sess-st", "task-1", "do something", ch);
    edc_task_set_state(task, TASK_WAITING_INPUT);
    edc_task_set_state(task, TASK_LLM_PROCESSING);
    edc_task_set_state(task, TASK_COMPLETED);
    task->is_group_leader = 1;
    task->output = sdsnew("task output result");
    task->completion_seq = 42;

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
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t0, "state")), "COMPLETED", "state COMPLETED");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(t0, "is_group_leader")), "is_group_leader");

    cJSON *turns_j = cJSON_GetObjectItem(t0, "turns");
    CHECK(cJSON_GetArraySize(turns_j) == 1, "1 turn serialized");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-st", "SingleTask");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    CHECK(count_groups(session2) == 1, "1 group restored");
    CHECK(count_tasks(session2) == 1, "1 task restored");

    struct edc_task *rtask = find_task(session2, "task-1");
    CHECK(rtask != NULL, "task-1 found after deserialize");
    CHECK(rtask->state == TASK_COMPLETED, "state remains COMPLETED");
    CHECK(rtask->is_group_leader == 1, "is_group_leader preserved");
    CHECK(rtask->completion_seq == 42, "completion_seq preserved");
    CHECK_STREQ(rtask->output, "task output result", "output preserved");

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

static void test_fork_chain(void)
{
    TEST("fork chain roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-fork", "Fork");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* Create parent task */
    struct edc_channel *ch = edc_channel_cli_new();
    struct edc_task *parent = edc_task_new("task-parent", "sess-fork", "task-parent",
                                            "parent task", ch);
    parent->is_group_leader = 1;
    edc_task_set_state(parent, TASK_WAITING_INPUT);
    edc_task_set_state(parent, TASK_LLM_PROCESSING);
    edc_task_set_state(parent, TASK_COMPLETED);

    /* Create fork children */
    struct edc_task *child1 = edc_task_fork(parent, "task-child1", "child 1", 10, NULL);
    edc_task_set_state(child1, TASK_WAITING_INPUT);
    edc_task_set_state(child1, TASK_LLM_PROCESSING);
    edc_task_set_state(child1, TASK_COMPLETED);

    struct edc_task *child2 = edc_task_fork(parent, "task-child2", "child 2", 10, NULL);
    edc_task_set_state(child2, TASK_WAITING_INPUT);

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
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
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
    CHECK(rc1->fork_point_msg_count == 10, "fork_point_msg_count preserved");

    /* Verify states */
    CHECK(rp->state == TASK_COMPLETED, "parent COMPLETED");
    CHECK(rc1->state == TASK_COMPLETED, "child1 COMPLETED");
    CHECK(rc2->state == TASK_WAITING_INPUT, "child2 WAITING_INPUT");

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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* Create tasks: A → (JOIN) → B → (JOIN) → C */
    struct edc_channel *ch = edc_channel_cli_new();
    struct edc_task *ta = edc_task_new("task-A", "sess-dep", "task-A", "source A", ch);
    ta->is_group_leader = 1;
    edc_task_set_state(ta, TASK_WAITING_INPUT);
    edc_task_set_state(ta, TASK_LLM_PROCESSING);
    edc_task_set_state(ta, TASK_COMPLETED);
    ta->output = sdsnew("output from A");

    struct edc_task *tb = edc_task_new("task-B", "sess-dep", "task-A", "middle B", NULL);
    edc_task_set_state(tb, TASK_WAITING_DEPS); /* must be set before adding deps */

    struct edc_task *tc = edc_task_new("task-C", "sess-dep", "task-A", "sink C", NULL);
    edc_task_set_state(tc, TASK_WAITING_DEPS);

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
    CHECK(tb->state == TASK_WAITING_INPUT, "B transitioned to WAITING_INPUT");

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-dep", "Dep");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
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
    CHECK(rb->state == TASK_WAITING_INPUT, "B in WAITING_INPUT");

    CHECK(rc->deps_total == 1, "C deps_total = 1");
    CHECK(rc->deps_completed == 0, "C deps_completed = 0 (B not done)");

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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* Group 1: 3 tasks */
    struct edc_task *g1_leader = edc_task_new("g1-L", "sess-cplx", "g1-L",
                                               "group1 leader", edc_channel_cli_new());
    g1_leader->is_group_leader = 1;
    edc_task_set_state(g1_leader, TASK_WAITING_INPUT);
    edc_task_set_state(g1_leader, TASK_LLM_PROCESSING);
    edc_task_set_state(g1_leader, TASK_COMPLETED);
    g1_leader->output = sdsnew("g1 leader done");

    struct edc_task *g1_m1 = edc_task_new("g1-M1", "sess-cplx", "g1-L",
                                           "group1 member1", NULL);
    edc_task_set_state(g1_m1, TASK_WAITING_INPUT);

    struct edc_task *g1_m2 = edc_task_new("g1-M2", "sess-cplx", "g1-L",
                                           "group1 member2", NULL);
    edc_task_set_state(g1_m2, TASK_IDLE); /* will be filtered out! */

    struct edc_task_group *group1 = edc_task_group_new(g1_leader);
    edc_task_group_add_task(group1, g1_m1);
    edc_task_group_add_task(group1, g1_m2);
    edc_session_add_group(session, group1);

    /* Group 2: 2 tasks, normal group */
    struct edc_task *g2_leader = edc_task_new("g2-L", "sess-cplx", "g2-L",
                                               "group2 leader", edc_channel_cli_new());
    g2_leader->is_group_leader = 1;
    edc_task_set_state(g2_leader, TASK_WAITING_INPUT);
    edc_task_set_state(g2_leader, TASK_LLM_PROCESSING);
    edc_task_set_state(g2_leader, TASK_COMPLETED);

    struct edc_task *g2_m1 = edc_task_new("g2-M1", "sess-cplx", "g2-L",
                                           "group2 member1", NULL);
    edc_task_set_state(g2_m1, TASK_WAITING_INPUT);

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

    /* Verify filtering: g1-M2 (IDLE) should NOT be in output */
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    int tcount = cJSON_GetArraySize(tasks_j);
    CHECK(tcount == 4, "4 tasks serialized (g1-L, g1-M1, g2-L, g2-M1; g1-M2 IDLE filtered)");

    /* Verify groups: 2 groups */
    cJSON *groups_j = cJSON_GetObjectItem(json, "groups");
    CHECK(cJSON_GetArraySize(groups_j) == 2, "2 groups serialized");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-cplx", "Complex");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    CHECK(count_groups(session2) == 2, "2 groups restored");
    CHECK(count_tasks(session2) == 4, "4 tasks restored (IDLE filtered)");

    struct edc_task *rg1L  = find_task(session2, "g1-L");
    struct edc_task *rg1M1 = find_task(session2, "g1-M1");
    struct edc_task *rg2L  = find_task(session2, "g2-L");
    struct edc_task *rg2M1 = find_task(session2, "g2-M1");
    struct edc_task *rg1M2 = find_task(session2, "g1-M2");

    CHECK(rg1L != NULL, "g1-L restored");
    CHECK(rg1M1 != NULL, "g1-M1 restored");
    CHECK(rg2L != NULL, "g2-L restored");
    CHECK(rg2M1 != NULL, "g2-M1 restored");
    CHECK(rg1M2 == NULL, "g1-M2 correctly filtered (IDLE)");

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
 *  Test 6: State degradation
 *  ══════════════════════════════════════════════════════════════ */

static void test_state_degradation(void)
{
    TEST("state degradation (LLM_PROCESSING/TOOL_CALLING → WAITING_INPUT)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-deg", "Degrade");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *t1 = edc_task_new("t-llm", "sess-deg", "t-llm",
                                        "llm processing", edc_channel_cli_new());
    t1->is_group_leader = 1;
    edc_task_set_state(t1, TASK_WAITING_INPUT);
    edc_task_set_state(t1, TASK_LLM_PROCESSING);
    /* stays in LLM_PROCESSING */

    struct edc_task *t2 = edc_task_new("t-tool", "sess-deg", "t-llm",
                                        "tool calling", NULL);
    edc_task_set_state(t2, TASK_WAITING_INPUT);
    edc_task_set_state(t2, TASK_LLM_PROCESSING);
    edc_task_set_state(t2, TASK_TOOL_CALLING);
    /* stays in TOOL_CALLING */

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
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(j1, "state")),
                "LLM_PROCESSING", "JSON keeps LLM_PROCESSING");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(j2, "state")),
                "TOOL_CALLING", "JSON keeps TOOL_CALLING");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-deg", "Degrade");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt1 = find_task(session2, "t-llm");
    struct edc_task *rt2 = find_task(session2, "t-tool");

    CHECK(rt1->state == TASK_WAITING_INPUT, "LLM_PROCESSING degraded to WAITING_INPUT");
    CHECK(rt2->state == TASK_WAITING_INPUT, "TOOL_CALLING degraded to WAITING_INPUT");

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
    TEST("IDLE and CANCELLED tasks are filtered out");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-filt", "Filter");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *leader = edc_task_new("t-ok", "sess-filt", "t-ok",
                                            "good task", edc_channel_cli_new());
    leader->is_group_leader = 1;
    edc_task_set_state(leader, TASK_WAITING_INPUT);
    edc_task_set_state(leader, TASK_LLM_PROCESSING);
    edc_task_set_state(leader, TASK_COMPLETED);

    struct edc_task *idle_t = edc_task_new("t-idle", "sess-filt", "t-ok",
                                            "idle task", NULL);
    /* stays IDLE */

    struct edc_task *canc_t = edc_task_new("t-canc", "sess-filt", "t-ok",
                                            "cancelled task", NULL);
    edc_task_set_state(canc_t, TASK_WAITING_INPUT);
    edc_task_set_state(canc_t, TASK_CANCELLED);

    struct edc_task_group *group = edc_task_group_new(leader);
    edc_task_group_add_task(group, idle_t);
    edc_task_group_add_task(group, canc_t);
    edc_session_add_group(session, group);

    /* Sanity: 3 tasks in session */
    CHECK(count_tasks(session) == 3, "3 tasks before serialize");

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    CHECK(cJSON_GetArraySize(tasks_j) == 1, "only 1 task serialized (t-ok)");

    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(t0, "task_id")),
                "t-ok", "the one task is t-ok");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-filt", "Filter");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");
    CHECK(count_tasks(session2) == 1, "1 task restored");

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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *task = edc_task_new("t-at", "sess-at", "t-at",
                                          "has active turn", edc_channel_cli_new());
    task->is_group_leader = 1;
    edc_task_set_state(task, TASK_WAITING_INPUT);
    edc_task_set_state(task, TASK_LLM_PROCESSING);

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
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
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
 *  Test 9: todolist, prompt, redirect fields
 *  ══════════════════════════════════════════════════════════════ */

static void test_extra_fields(void)
{
    TEST("todolist, prompt, redirect roundtrip");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-extra", "Extra");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *task = edc_task_new("t-extra", "sess-extra", "t-extra",
                                          "extra fields", edc_channel_cli_new());
    task->is_group_leader = 1;
    edc_task_set_state(task, TASK_WAITING_INPUT);
    edc_task_set_state(task, TASK_LLM_PROCESSING);
    edc_task_set_state(task, TASK_COMPLETED);

    /* Set prompt */
    task->prompt_name = sdsnew("code-reviewer");
    task->prompt = sdsnew("You are a senior code reviewer.");

    /* Set redirect */
    task->redirect_stdout = sdsnew("/tmp/out.log");
    task->redirect_stderr = sdsnew("/tmp/err.log");
    task->redirect_append = true;

    /* Set todolist */
    task->todolist = cJSON_CreateArray();
    cJSON *item1 = cJSON_CreateObject();
    cJSON_AddStringToObject(item1, "content", "review code");
    cJSON_AddStringToObject(item1, "status", "completed");
    cJSON_AddStringToObject(item1, "activeForm", "");
    cJSON_AddItemToArray(task->todolist, item1);
    cJSON *item2 = cJSON_CreateObject();
    cJSON_AddStringToObject(item2, "content", "write tests");
    cJSON_AddStringToObject(item2, "status", "in_progress");
    cJSON_AddStringToObject(item2, "activeForm", "writing...");
    cJSON_AddItemToArray(task->todolist, item2);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize returns non-NULL");

    /* Deserialize */
    struct edc_session *session2 = edc_session_new(cfg, "sess-extra", "Extra");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt = find_task(session2, "t-extra");
    CHECK(rt != NULL, "task restored");

    /* Verify prompt */
    CHECK_STREQ(rt->prompt_name, "code-reviewer", "prompt_name preserved");
    CHECK_STREQ(rt->prompt, "You are a senior code reviewer.", "prompt preserved");

    /* Verify redirect */
    CHECK_STREQ(rt->redirect_stdout, "/tmp/out.log", "redirect_stdout preserved");
    CHECK_STREQ(rt->redirect_stderr, "/tmp/err.log", "redirect_stderr preserved");
    CHECK(rt->redirect_append == true, "redirect_append preserved");

    /* Verify todolist */
    CHECK(rt->todolist != NULL, "todolist restored");
    CHECK(cJSON_GetArraySize(rt->todolist) == 2, "todolist has 2 items");
    cJSON *ri1 = cJSON_GetArrayItem(rt->todolist, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(ri1, "content")),
                "review code", "todolist item1 content");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(ri1, "status")),
                "completed", "todolist item1 status");

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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* serialize with NULL session */
    cJSON *j1 = edc_state_serialize(NULL, NULL);
    CHECK(j1 == NULL, "serialize(NULL, NULL) returns NULL");

    /* serialize with NULL messages */
    cJSON *saved = session->messages;
    session->messages = NULL;
    cJSON *j2 = edc_state_serialize(session, NULL);
    CHECK(j2 == NULL, "serialize with NULL messages returns NULL");
    session->messages = saved;

    /* deserialize with NULL json */
    bool ok1 = edc_state_deserialize(NULL, session, NULL);
    CHECK(!ok1, "deserialize(NULL json) returns false");

    /* deserialize with NULL session */
    cJSON *valid = cJSON_Parse("{\"version\":1,\"session\":{},\"groups\":[],\"tasks\":[]}");
    bool ok2 = edc_state_deserialize(valid, NULL, NULL);
    CHECK(!ok2, "deserialize(NULL session) returns false");
    cJSON_Delete(valid);

    /* deserialize with missing version */
    cJSON *no_ver = cJSON_Parse("{\"session\":{},\"groups\":[],\"tasks\":[]}");
    bool ok3 = edc_state_deserialize(no_ver, session, sched);
    CHECK(!ok3, "deserialize without version returns false");
    cJSON_Delete(no_ver);

    /* deserialize with missing tasks array */
    cJSON *no_tasks = cJSON_Parse("{\"version\":1,\"session\":{},\"groups\":[]}");
    bool ok4 = edc_state_deserialize(no_tasks, session, sched);
    CHECK(ok4, "deserialize without tasks array succeeds (treated as empty)");
    cJSON_Delete(no_tasks);

    /* deserialize with tasks not an array */
    cJSON *bad_tasks = cJSON_Parse("{\"version\":1,\"session\":{},\"groups\":[],\"tasks\":\"not_array\"}");
    bool ok5 = edc_state_deserialize(bad_tasks, session, sched);
    CHECK(ok5, "deserialize with non-array tasks succeeds (ignored)");
    cJSON_Delete(bad_tasks);

    /* deserialize with groups not an array */
    cJSON *bad_groups = cJSON_Parse("{\"version\":1,\"session\":{},\"groups\":123,\"tasks\":[]}");
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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* dep referencing nonexistent task */
    const char *bad_dep_json =
        "{"
        "  \"version\":1,"
        "  \"session\":{},"
        "  \"groups\":["
        "    {\"group_id\":\"g1\",\"leader_task_id\":\"t1\","
        "     \"member_task_ids\":[\"t1\"],\"is_orphaned\":false}"
        "  ],"
        "  \"tasks\":["
        "    {\"task_id\":\"t1\",\"group_id\":\"g1\",\"session_id\":\"sess-mal\","
        "     \"description\":\"task 1\",\"state\":\"COMPLETED\","
        "     \"is_group_leader\":true,\"auto_reap\":false,"
        "     \"fork_point_msg_count\":0,\"completion_seq\":0,"
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
        "  \"version\":1,"
        "  \"session\":{},"
        "  \"groups\":["
        "    {\"group_id\":\"g1\",\"leader_task_id\":\"t1\","
        "     \"member_task_ids\":[\"t1\",\"NOBODY\"],\"is_orphaned\":false}"
        "  ],"
        "  \"tasks\":["
        "    {\"task_id\":\"t1\",\"group_id\":\"g1\",\"session_id\":\"sess-mal\","
        "     \"description\":\"task 1\",\"state\":\"COMPLETED\","
        "     \"is_group_leader\":true,\"auto_reap\":false,"
        "     \"fork_point_msg_count\":0,\"completion_seq\":0,"
        "     \"redirect_append\":false,"
        "     \"deps\":[],\"turns\":[]}"
        "  ]"
        "}";
    cJSON *j2 = cJSON_Parse(bad_member_json);
    bool ok2 = edc_state_deserialize(j2, session, sched);
    CHECK(ok2, "deserialize with bad member reference succeeds (skipped)");
    cJSON_Delete(j2);

    /* turn message_start out of range */
    const char *bad_range_json =
        "{"
        "  \"version\":1,"
        "  \"session\":{},"
        "  \"groups\":["
        "    {\"group_id\":\"g1\",\"leader_task_id\":\"t1\","
        "     \"member_task_ids\":[\"t1\"],\"is_orphaned\":false}"
        "  ],"
        "  \"tasks\":["
        "    {\"task_id\":\"t1\",\"group_id\":\"g1\",\"session_id\":\"sess-mal\","
        "     \"description\":\"task 1\",\"state\":\"COMPLETED\","
        "     \"is_group_leader\":true,\"auto_reap\":false,"
        "     \"fork_point_msg_count\":0,\"completion_seq\":0,"
        "     \"redirect_append\":false,"
        "     \"deps\":[],"
        "     \"turns\":[{\"turn_index\":0,\"message_start\":999,\"message_count\":5}]}"
        "  ]"
        "}";
    cJSON *j3 = cJSON_Parse(bad_range_json);
    bool ok3 = edc_state_deserialize(j3, session, sched);
    CHECK(ok3, "deserialize with out-of-range message_start succeeds (turn empty)");
    cJSON_Delete(j3);

    /* task with empty task_id */
    const char *empty_id_json =
        "{"
        "  \"version\":1,"
        "  \"session\":{},"
        "  \"groups\":[],"
        "  \"tasks\":["
        "    {\"task_id\":\"\",\"group_id\":\"g1\",\"session_id\":\"s\","
        "     \"description\":\"\",\"state\":\"COMPLETED\","
        "     \"is_group_leader\":false,\"auto_reap\":false,"
        "     \"fork_point_msg_count\":0,\"completion_seq\":0,"
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
    TEST("compact message reference (messages in session->messages)");

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-cpt", "Compact");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* Build turn messages that also exist in session->messages.
     * We add messages to session->messages first, then add the SAME
     * pointers to a turn via cJSON_AddItemReferenceToArray
     * (which doesn't take ownership).  This simulates what happens
     * when edc_session_add_turn moves messages from turn to session. */

    struct edc_task *task = edc_task_new("t-cpt", "sess-cpt", "t-cpt",
                                          "compact test", edc_channel_cli_new());
    task->is_group_leader = 1;
    edc_task_set_state(task, TASK_WAITING_INPUT);
    edc_task_set_state(task, TASK_LLM_PROCESSING);
    edc_task_set_state(task, TASK_COMPLETED);

    /* Create messages and add to session->messages */
    cJSON *msg1 = cJSON_CreateObject();
    cJSON_AddStringToObject(msg1, "role", "user");
    cJSON_AddStringToObject(msg1, "content", "compact query");
    cJSON_AddItemToArray(session->messages, msg1);

    cJSON *msg2 = cJSON_CreateObject();
    cJSON_AddStringToObject(msg2, "role", "assistant");
    cJSON_AddStringToObject(msg2, "content", "compact answer");
    cJSON_AddItemToArray(session->messages, msg2);

    /* Build turn with references to the same objects */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("t-cpt");
    /* Push references (not owned by turn) */
    edc_queue_push(&turn->messages, msg1);
    edc_queue_push(&turn->messages, msg2);
    edc_queue_push(&turn->timestamps, sdsnew("2026-01-01T00:00:00"));
    edc_queue_push(&turn->timestamps, sdsnew("2026-01-01T00:00:01"));
    edc_task_add_turn(task, turn);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* SERIALIZE — should take compact path (pointer match) */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds with compact path");

    /* Verify compact mode was used */
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *turns_j = cJSON_GetObjectItem(t0, "turns");
    cJSON *turn_j = cJSON_GetArrayItem(turns_j, 0);

    cJSON *embedded  = cJSON_GetObjectItem(turn_j, "messages");
    cJSON *msg_start = cJSON_GetObjectItem(turn_j, "message_start");
    cJSON *msg_count = cJSON_GetObjectItem(turn_j, "message_count");

    CHECK(embedded == NULL, "compact mode: no embedded messages array");
    CHECK(msg_start != NULL, "compact mode: has message_start");
    CHECK(msg_count != NULL, "compact mode: has message_count");
    CHECK((int)cJSON_GetNumberValue(msg_count) == 2, "message_count = 2");

    /* Verify the start index (should be after system prompt, so >= 1) */
    int start = (int)cJSON_GetNumberValue(msg_start);
    CHECK(start >= 1, "message_start >= 1 (after system prompt)");

    /* DESERIALIZE — need matching messages in target session */
    struct edc_session *session2 = edc_session_new(cfg, "sess-cpt", "Compact");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);

    /* Put identical-content messages at the same positions in session2 */
    cJSON *msg1b = cJSON_CreateObject();
    cJSON_AddStringToObject(msg1b, "role", "user");
    cJSON_AddStringToObject(msg1b, "content", "compact query");
    cJSON_AddItemToArray(session2->messages, msg1b);

    cJSON *msg2b = cJSON_CreateObject();
    cJSON_AddStringToObject(msg2b, "role", "assistant");
    cJSON_AddStringToObject(msg2b, "content", "compact answer");
    cJSON_AddItemToArray(session2->messages, msg2b);

    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds from compact mode");

    struct edc_task *rt = find_task(session2, "t-cpt");
    CHECK(rt != NULL, "task restored from compact mode");
    CHECK(edc_queue_length(&rt->turns) == 1, "1 turn from compact mode");

    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(edc_queue_length(&rturn->messages) == 2, "2 messages from compact range");

    cJSON *m0 = (cJSON *)rturn->messages.data[rturn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")),
                "compact query", "msg0 content correct in compact roundtrip");

    /* Cleanup: clear turn queues manually to avoid double-free
     * (msg1/msg2 are owned by session->messages, not by turn) */
    while (edc_queue_length(&turn->messages) > 0)
        edc_queue_pop(&turn->messages);
    while (edc_queue_length(&turn->timestamps) > 0)
        sdsfree((sds)edc_queue_pop(&turn->timestamps));
    /* Also clear task->turns to prevent edc_task_free from freeing the turn */
    while (edc_queue_length(&task->turns) > 0) {
        struct edc_turn *t = edc_queue_pop(&task->turns);
        while (edc_queue_length(&t->messages) > 0) edc_queue_pop(&t->messages);
        while (edc_queue_length(&t->timestamps) > 0) sdsfree(edc_queue_pop(&t->timestamps));
        free(t->task_id);
        free(t);
    }

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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *task = edc_task_new("t-et", "sess-et", "t-et",
                                          "empty turn task", edc_channel_cli_new());
    task->is_group_leader = 1;
    edc_task_set_state(task, TASK_WAITING_INPUT);
    edc_task_set_state(task, TASK_LLM_PROCESSING);
    edc_task_set_state(task, TASK_COMPLETED);

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
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItem(turn_j, "message_count")) == 0,
          "message_count = 0");

    struct edc_session *session2 = edc_session_new(cfg, "sess-et", "EmptyTurn");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
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
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* Create group with leader + 2 members */
    struct edc_task *leader = edc_task_new("t-leader", "sess-orph", "t-leader",
                                            "group leader", edc_channel_cli_new());
    leader->is_group_leader = 1;
    edc_task_set_state(leader, TASK_WAITING_INPUT);
    edc_task_set_state(leader, TASK_LLM_PROCESSING);
    edc_task_set_state(leader, TASK_COMPLETED);

    struct edc_task *m1 = edc_task_new("t-m1", "sess-orph", "t-leader",
                                        "member 1", NULL);
    edc_task_set_state(m1, TASK_WAITING_INPUT);

    struct edc_task *m2 = edc_task_new("t-m2", "sess-orph", "t-leader",
                                        "member 2", NULL);
    edc_task_set_state(m2, TASK_WAITING_INPUT);

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
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);
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

int main(void)
{
    log_info("=== Phase 1: edc_state_serializer module tests ===");
    log_info("");

    test_empty_session();
    test_single_task();
    test_fork_chain();
    test_dependency_graph();
    test_complex_topology();
    test_state_degradation();
    test_filter_transient_states();
    test_active_turn();
    test_extra_fields();
    test_null_inputs();
    test_malformed_inputs();
    test_compact_messages();
    test_empty_turn();
    test_orphaned_group();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
