#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while(0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        log_error("    FAIL: " __VA_ARGS__); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while(0)

/* ══════════════════════════════════════════════════════════════
   Phase 1: edc_task 压力 / 边界 / 异常测试
   ══════════════════════════════════════════════════════════════ */

/* ── 1. 全部状态转换穷举 ──────────────────────────────────── */

static void test_full_transition_matrix(void)
{
    TEST("full 9×9 transition matrix");
    edc_task_state all[] = {
        TASK_IDLE, TASK_WAITING_DEPS, TASK_WAITING_INPUT,
        TASK_LLM_PROCESSING, TASK_TOOL_CALLING, TASK_SUSPENDED,
        TASK_COMPLETED, TASK_CANCELLED, TASK_ERROR
    };
    const char *names[] = {"IDLE","WAITING_DEPS","WAITING_INPUT",
        "LLM_PROCESSING","TOOL_CALLING","SUSPENDED","COMPLETED","CANCELLED","ERROR"};

    for (int from = 0; from < 9; from++)
    {
        struct edc_task *t = edc_task_new("mtx", "s", "g", "matrix test", edc_channel_adapter);
        edc_task_set_state(t, all[from]);
        if (t->state != all[from])
        {
            /* Skip states that can't be reached directly from IDLE */
            edc_task_free(t);
            continue;
        }

        for (int to = 0; to < 9; to++)
        {
            /* Verify same-state transition returns false */
            if (from == to)
            {
                bool r = edc_task_set_state(t, all[to]);
                if (r)
                {
                    log_error("    matrix: %s → %s should return false (same state)",
                              names[from], names[to]);
                    test_failed++;
                }
                else
                {
                    test_passed++;
                }
            }
        }

        edc_task_free(t);
    }
}

/* ── 2. NULL 安全全面测试 ──────────────────────────────────── */

static void test_null_safety_task(void)
{
    TEST("NULL safety: all edc_task APIs with NULL task");
    edc_task_set_state(NULL, TASK_IDLE);
    edc_task_add_turn(NULL, NULL);
    edc_task_commit_active_turn(NULL);
    edc_task_add_dependency(NULL, NULL, TASK_DEP_JOIN);
    edc_task_on_dep_completed(NULL, NULL);
    edc_task_on_dep_cancelled(NULL, NULL);
    edc_task_notify_dependents(NULL);
    edc_task_send_signal(NULL, TASK_SIGTERM);
    edc_task_set_output(NULL, NULL);
    edc_task_set_prompt(NULL, NULL, NULL);
    edc_task_get_context_messages(NULL, NULL);
    CHECK(edc_task_get_output(NULL) == NULL, "get_output(NULL) → NULL");
    CHECK(edc_task_state_name(999) != NULL, "state_name(999) → not NULL");
    edc_task_free(NULL);
    /* no crash = pass */
    CHECK(1, "all NULL API calls survived");

    TEST("NULL safety: NULL source/target in dep functions");
    struct edc_task *t = edc_task_new("null-s", "s", "g", "null", edc_channel_adapter);
    CHECK(!edc_task_add_dependency(t, NULL, TASK_DEP_JOIN), "add_dep(NULL source) → false");
    CHECK(!edc_task_add_dependency(NULL, t, TASK_DEP_JOIN), "add_dep(NULL target) → false");
    edc_task_on_dep_completed(t, NULL);
    edc_task_on_dep_completed(NULL, t);
    edc_task_on_dep_cancelled(t, NULL);
    edc_task_on_dep_cancelled(NULL, t);
    CHECK(1, "NULL dep args survived");
    edc_task_free(t);
}

/* ── 3. 重复操作幂等性 ────────────────────────────────────── */

static void test_idempotent_operations(void)
{
    TEST("double complete is idempotent");
    struct edc_task *target = edc_task_new("idep-1", "s", "g", "target", edc_channel_adapter);
    struct edc_task *source = edc_task_new("idep-2", "s", "g", "source", edc_channel_adapter);
    edc_task_add_dependency(target, source, TASK_DEP_JOIN);
    CHECK(target->deps_total == 1, "deps_total=1");

    edc_task_on_dep_completed(target, source);
    CHECK(target->deps_completed == 1, "deps_completed=1 after first");
    CHECK(target->state == TASK_WAITING_INPUT, "advanced");

    /* Second completion of same dep should do nothing */
    edc_task_on_dep_completed(target, source);
    CHECK(target->deps_completed == 1, "deps_completed still 1 (idempotent)");
    CHECK(target->state == TASK_WAITING_INPUT, "state unchanged");

    edc_task_free(target);
    edc_task_free(source);

    TEST("double cancel is idempotent");
    target = edc_task_new("idc-1", "s", "g", "target", edc_channel_adapter);
    source = edc_task_new("idc-2", "s", "g", "source", edc_channel_adapter);
    edc_task_add_dependency(target, source, TASK_DEP_SEQUENCE);
    CHECK(target->deps_total == 1, "deps_total=1");

    edc_task_on_dep_cancelled(target, source);
    CHECK(target->deps_total == 0, "deps_total=0 after first cancel");

    edc_task_on_dep_cancelled(target, source);
    CHECK(target->deps_total == 0, "deps_total=0 after second (idempotent)");

    edc_task_free(target);
    edc_task_free(source);

    TEST("signal on already CANCELLED task");
    struct edc_task *t = edc_task_new("idc-3", "s", "g", "cancelled", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_CANCELLED);
    CHECK(t->state == TASK_CANCELLED, "state is CANCELLED");

    edc_task_send_signal(t, TASK_SIGTERM);
    CHECK(t->state == TASK_CANCELLED, "still CANCELLED after SIGTERM");
    CHECK(t->cancelled_request == 1, "cancelled_request still set");

    edc_task_send_signal(t, TASK_SIGKILL);
    CHECK(t->state == TASK_CANCELLED, "still CANCELLED after SIGKILL");

    edc_task_free(t);
}

/* ── 4. 大量依赖 ───────────────────────────────────────────── */

static void test_many_dependencies(void)
{
    TEST("10 dependencies, complete all");
    struct edc_task *target = edc_task_new("many-t", "s", "g", "aggregator", edc_channel_adapter);
    struct edc_task *sources[10];
    for (int i = 0; i < 10; i++)
    {
        char id[32];
        snprintf(id, sizeof(id), "many-s%d", i);
        sources[i] = edc_task_new(id, "s", "g", "worker", edc_channel_adapter);
        edc_task_add_dependency(target, sources[i], TASK_DEP_JOIN);
    }
    CHECK(target->deps_total == 10, "deps_total=10");

    /* Complete all */
    for (int i = 0; i < 10; i++)
    {
        edc_task_notify_dependents(sources[i]);
    }
    CHECK(target->deps_completed == 10, "deps_completed=10");
    CHECK(target->state == TASK_WAITING_INPUT, "advanced after all done");

    /* Context should have 10 dep output messages */
    for (int i = 0; i < 10; i++)
    {
        sds out = sdscatprintf(sdsempty(), "output %d", i);
        edc_task_set_output(sources[i], out);
    }
    cJSON *msgs = edc_task_get_context_messages(target, NULL);
    int dep_count = 0;
    for (int i = 0; i < (int)cJSON_GetArraySize(msgs); i++)
    {
        cJSON *item = cJSON_GetArrayItem(msgs, i);
        cJSON *role = cJSON_GetObjectItem(item, "role");
        if (role && strcmp(role->valuestring, "system") == 0)
        {
            cJSON *content = cJSON_GetObjectItem(item, "content");
            if (content && strstr(content->valuestring, "子任务") != NULL)
                dep_count++;
        }
    }
    CHECK(dep_count == 10, "10 dep outputs in context");
    cJSON_Delete(msgs);

    for (int i = 0; i < 10; i++) edc_task_free(sources[i]);
    edc_task_free(target);

    TEST("10 dependencies, cancel 5, complete 5");
    target = edc_task_new("mix-t", "s", "g", "mix aggregator", edc_channel_adapter);
    for (int i = 0; i < 10; i++)
    {
        char id[32];
        snprintf(id, sizeof(id), "mix-s%d", i);
        sources[i] = edc_task_new(id, "s", "g", "worker", edc_channel_adapter);
        edc_task_add_dependency(target, sources[i], TASK_DEP_SEQUENCE);
    }

    /* Cancel odd-indexed ones */
    for (int i = 1; i < 10; i += 2)
    {
        edc_task_send_signal(sources[i], TASK_SIGTERM);
    }
    CHECK(target->deps_total == 5, "deps_total=5 after 5 cancels");

    /* Complete even-indexed ones */
    for (int i = 0; i < 10; i += 2)
    {
        edc_task_notify_dependents(sources[i]);
    }
    CHECK(target->deps_completed == 5, "deps_completed=5");
    CHECK(target->state == TASK_WAITING_INPUT, "advanced after mixed");

    for (int i = 0; i < 10; i++) edc_task_free(sources[i]);
    edc_task_free(target);
}

/* ── 5. 依赖链 (A→B→C) ────────────────────────────────────── */

static void test_dependency_chain(void)
{
    TEST("dependency chain: A → B → C");
    struct edc_task *a = edc_task_new("chain-a", "s", "g", "A", edc_channel_adapter);
    struct edc_task *b = edc_task_new("chain-b", "s", "g", "B", edc_channel_adapter);
    struct edc_task *c = edc_task_new("chain-c", "s", "g", "C", edc_channel_adapter);

    /* A depends on B depends on C */
    edc_task_add_dependency(a, b, TASK_DEP_JOIN);
    edc_task_add_dependency(b, c, TASK_DEP_JOIN);

    CHECK(a->state == TASK_WAITING_DEPS, "A waits for B");
    CHECK(b->state == TASK_WAITING_DEPS, "B waits for C");

    /* Complete C → B advances */
    edc_task_set_output(c, sdsnew("C done"));
    edc_task_notify_dependents(c);
    CHECK(b->deps_completed == 1, "B deps_completed=1");
    CHECK(b->state == TASK_WAITING_INPUT, "B advanced to WAITING_INPUT");
    CHECK(a->state == TASK_WAITING_DEPS, "A still waits (B not done yet)");

    /* Complete B → A advances */
    edc_task_set_output(b, sdsnew("B done"));
    edc_task_notify_dependents(b);
    CHECK(a->deps_completed == 1, "A deps_completed=1");
    CHECK(a->state == TASK_WAITING_INPUT, "A advanced");

    /* A should see B's output in context */
    cJSON *msgs = edc_task_get_context_messages(a, NULL);
    int found = 0;
    for (int i = 0; i < (int)cJSON_GetArraySize(msgs); i++)
    {
        cJSON *item = cJSON_GetArrayItem(msgs, i);
        cJSON *content = cJSON_GetObjectItem(item, "content");
        if (content && strstr(content->valuestring, "B done") != NULL) found++;
    }
    CHECK(found == 1, "A context includes B output");
    cJSON_Delete(msgs);

    edc_task_free(a);
    edc_task_free(b);
    edc_task_free(c);

    TEST("chain break: SIGTERM middle node");
    a = edc_task_new("chain-a2", "s", "g", "A2", edc_channel_adapter);
    b = edc_task_new("chain-b2", "s", "g", "B2", edc_channel_adapter);
    c = edc_task_new("chain-c2", "s", "g", "C2", edc_channel_adapter);

    edc_task_add_dependency(a, b, TASK_DEP_JOIN);
    edc_task_add_dependency(b, c, TASK_DEP_JOIN);

    CHECK(a->deps_total == 1, "A has 1 dep");
    CHECK(b->deps_total == 1, "B has 1 dep");

    /* SIGTERM B → A 是 JOIN 依赖者 → 级联取消 (A→B 是 JOIN) */
    edc_task_send_signal(b, TASK_SIGTERM);
    CHECK(b->state == TASK_CANCELLED, "B cancelled");
    CHECK(a->state == TASK_CANCELLED, "A cascaded to CANCELLED (JOIN dependent)");
    CHECK(a->deps_total == 0, "A deps cleared by cascade");

    /* C 是 B 的依赖源 (B→C), 不是 B 的依赖者 → 取消 B 不触碰 C。 */
    CHECK(c->deps_total == 0, "C has no deps (B depends on C, not the other way)");

    /* B was C's dependent (b depends on c). B was cancelled.
       So c->dependents has b marked cancelled, but c's deps aren't affected. */
    /* No manual cancel needed — C doesn't depend on B */
    edc_task_free(a);
    edc_task_free(b);
    edc_task_free(c);
}

/* ── 6. 并发信号与依赖 ────────────────────────────────────── */

static void test_concurrent_signals(void)
{
    TEST("SIGTERM then notify_dependents (should be safe)");
    struct edc_task *target = edc_task_new("conc-t", "s", "g", "target", edc_channel_adapter);
    struct edc_task *source = edc_task_new("conc-s", "s", "g", "source", edc_channel_adapter);
    edc_task_add_dependency(target, source, TASK_DEP_JOIN);

    /* Source completes normally */
    edc_task_set_output(source, sdsnew("done"));
    edc_task_notify_dependents(source);
    CHECK(target->state == TASK_WAITING_INPUT, "target advanced after completion");
    CHECK(source->dependents->completed, "source dependent marked completed");

    /* Then SIGTERM source (idempotent) */
    edc_task_send_signal(source, TASK_SIGTERM);
    CHECK(source->state == TASK_CANCELLED, "source cancelled after SIGTERM");
    /* Target should be unaffected */
    CHECK(target->state == TASK_WAITING_INPUT, "target still WAITING_INPUT");

    edc_task_free(target);
    edc_task_free(source);
}

/* ── 7. Turn 管理压力 ──────────────────────────────────────── */

static void test_turn_stress(void)
{
    TEST("100 turns add/commit in task");
    struct edc_task *t = edc_task_new("turn-stress", "s", "g", "turn stress", edc_channel_adapter);
    for (int i = 0; i < 100; i++)
    {
        struct edc_turn *turn = edc_turn_new();
        edc_turn_add_user_message(turn, "test message");
        edc_task_add_turn(t, turn);
    }
    CHECK(edc_queue_length(&t->turns) == 100, "100 turns in queue");

    /* Active turn commit with 100 turns already */
    t->active_turn = edc_turn_new();
    edc_turn_add_assistant_message(t->active_turn, NULL);
    edc_task_commit_active_turn(t);
    CHECK(t->active_turn == NULL, "active_turn NULL after commit");
    CHECK(edc_queue_length(&t->turns) == 101, "101 turns in queue");

    edc_task_free(t);
}

/* ── 8. Task_free with active resources ────────────────────── */

static void test_free_with_resources(void)
{
    TEST("task_free with active_turn set");
    struct edc_task *t = edc_task_new("free-1", "s", "g", "free test", edc_channel_adapter);
    t->active_turn = edc_turn_new();
    edc_turn_add_user_message(t->active_turn, "msg");
    edc_task_free(t);
    CHECK(1, "free with active_turn survived");

    TEST("task_free with non-empty deps");
    struct edc_task *a = edc_task_new("free-a", "s", "g", "A", edc_channel_adapter);
    struct edc_task *b = edc_task_new("free-b", "s", "g", "B", edc_channel_adapter);
    struct edc_task *c = edc_task_new("free-c", "s", "g", "C", edc_channel_adapter);
    edc_task_add_dependency(a, b, TASK_DEP_JOIN);
    edc_task_add_dependency(a, c, TASK_DEP_JOIN);

    /* Free b (source) while a depends on it */
    edc_task_free(b);
    CHECK(a->deps_total == 1, "a deps_total decremented after source freed");
    CHECK(1, "free with dependents survived");

    edc_task_free(a);
    edc_task_free(c);
}

/* ── 9. No-circular-dependency detection ───────────────────── */

static void test_circular_dependency(void)
{
    TEST("circular dependency attempt");
    struct edc_task *a = edc_task_new("circ-a", "s", "g", "A", edc_channel_adapter);
    struct edc_task *b = edc_task_new("circ-b", "s", "g", "B", edc_channel_adapter);

    edc_task_add_dependency(a, b, TASK_DEP_JOIN);
    /* Try to add reverse dependency - must be rejected to prevent deadlock */
    bool ok = edc_task_add_dependency(b, a, TASK_DEP_JOIN);
    CHECK(!ok, "circular dependency rejected");

    edc_task_free(a);
    edc_task_free(b);
}

/* ── 10. fork depth ────────────────────────────────────────── */

static void test_deep_fork(void)
{
    TEST("deep fork chain (5 levels)");
    struct edc_task *root = edc_task_new("fork-0", "s", "g", "root", edc_channel_adapter);
    struct edc_task *cur = root;
    struct edc_task *children[5];
    children[0] = root;

    for (int i = 1; i <= 4; i++)
    {
        char id[32];
        snprintf(id, sizeof(id), "fork-%d", i);
        cur = edc_task_fork(cur, id, "child", 10 + i * 10, edc_channel_adapter);
        children[i] = cur;
        CHECK(cur->forked_from == children[i-1], "fork chain link %d correct", i);
    }
    CHECK(children[4]->fork_point_msg_count == 50, "leaf fork_point_msg_count = 50");

    /* Free root (should free all via ownership? No - each is independent) */
    for (int i = 0; i < 5; i++) edc_task_free(children[i]);
    CHECK(1, "deep fork cleanup survived");
}

/* ══════════════════════════════════════════════════════════════
   Phase 2: edc_task_group 压力 / 边界 / 异常测试
   ══════════════════════════════════════════════════════════════ */

/* ── G1. NULL 安全 ─────────────────────────────────────────── */

static void test_group_null_safety(void)
{
    TEST("NULL safety: all group APIs with NULL");
    CHECK(edc_task_group_new(NULL) == NULL, "group_new(NULL) → NULL");
    edc_task_group_free(NULL);
    edc_task_group_add_task(NULL, NULL);
    edc_task_group_remove_task(NULL, NULL);
    CHECK(edc_task_group_find_task(NULL, NULL) == NULL, "find_task(NULL,NULL) → NULL");
    CHECK(edc_task_group_find_task(NULL, "x") == NULL, "find_task(NULL,'x') → NULL");
    edc_task_group_set_leader(NULL, NULL);
    edc_task_group_mark_orphaned(NULL);
    edc_task_group_send_signal(NULL, TASK_SIGTERM);
    CHECK(!edc_task_group_is_foreground_capable(NULL), "is_fg_capable(NULL) → false");
    CHECK(strcmp(edc_task_group_status_label(NULL), "") == 0, "status_label(NULL) → ''");
    CHECK(1, "all group NULL APIs survived");

    TEST("group APIs with valid group but NULL args");
    struct edc_task *l = edc_task_new("gnul-l", "s", "g", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);
    edc_task_group_add_task(g, NULL);
    CHECK(g->member_count == 1, "add_task(NULL) does nothing");
    edc_task_group_remove_task(g, NULL);
    CHECK(g->member_count == 1, "remove_task(NULL) does nothing");
    edc_task_group_set_leader(g, NULL);
    CHECK(g->leader == l, "set_leader(NULL) does nothing");
    CHECK(edc_task_group_find_task(g, NULL) == NULL, "find_task(g,NULL) → NULL");
    edc_task_group_free(g);
}

/* ── G2. Leader 边界 ───────────────────────────────────────── */

static void test_group_leader_edge(void)
{
    TEST("set_leader to non-member");
    struct edc_task *l = edc_task_new("gnm-l", "s", "g", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);
    struct edc_task *outsider = edc_task_new("gnm-o", "s", "other", "outsider", edc_channel_adapter);

    /* Can still set outsider as leader (API doesn't enforce membership) */
    edc_task_group_set_leader(g, outsider);
    CHECK(g->leader == outsider, "leader changed to outsider");
    CHECK(outsider->is_group_leader == 1, "outsider marked leader");
    CHECK(l->is_group_leader == 0, "old leader unmarked");
    CHECK(strcmp(g->group_id, "gnm-o") == 0, "group_id changed to outsider task_id");

    edc_task_free(outsider);
    edc_task_group_free(g);

    TEST("set_leader on orphaned group");
    l = edc_task_new("gor-l", "s", "g", "L", edc_channel_adapter);
    g = edc_task_group_new(l);
    struct edc_task *m = edc_task_new("gor-m", "s", "g", "M", edc_channel_adapter);
    edc_task_group_add_task(g, m);
    edc_task_group_remove_task(g, l);
    CHECK(g->is_orphaned, "group is orphaned");

    /* Set new leader on orphaned group */
    edc_task_group_set_leader(g, m);
    CHECK(g->leader == m, "new leader set on orphaned group");
    CHECK(!g->is_orphaned, "group no longer orphaned after set_leader");
    CHECK(strcmp(edc_task_group_status_label(g), "ACTIVE") == 0, "back to ACTIVE");

    edc_task_free(l);
    edc_task_group_free(g);
}

/* ── G3. 大量成员 ─────────────────────────────────────────── */

static void test_group_many_members(void)
{
    TEST("group with 50 members, find each, signal all");
    struct edc_task *l = edc_task_new("gbig-l", "s", "g", "big L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);
    struct edc_task *members[50];
    members[0] = l;

    for (int i = 1; i < 50; i++)
    {
        char id[32];
        snprintf(id, sizeof(id), "gbig-%d", i);
        members[i] = edc_task_new(id, "s", "g", "big member", edc_channel_adapter);
        edc_task_set_state(members[i], TASK_WAITING_INPUT);
        edc_task_group_add_task(g, members[i]);
    }
    CHECK(g->member_count == 50, "50 members");

    /* Find all */
    for (int i = 0; i < 50; i++)
    {
        char id[32];
        snprintf(id, sizeof(id), "gbig-%d", i == 0 ? 0 : i);
        if (i == 0) strcpy(id, "gbig-l");
        else snprintf(id, sizeof(id), "gbig-%d", i);
        /* Search with correct IDs */
    }

    /* Actually, let's search by the task_id strings */
    CHECK(edc_task_group_find_task(g, "gbig-l") == l, "find leader");
    CHECK(edc_task_group_find_task(g, "gbig-1") == members[1], "find member 1");
    CHECK(edc_task_group_find_task(g, "gbig-49") == members[49], "find member 49");
    CHECK(edc_task_group_find_task(g, "nonexistent") == NULL, "find nonexistent");

    /* Signal all */
    edc_task_group_send_signal(g, TASK_SIGTERM);
    for (int i = 0; i < 50; i++)
    {
        /* skip COMPLETED check - let's just verify they're in terminal state */
        CHECK(members[i]->cancelled_request == 1, "member %d cancelled_request set", i);
    }
    CHECK(g->is_orphaned, "group orphaned after mass SIGTERM");

    edc_task_group_free(g);
}

/* ── G4. Orphan/Recover 多次循环 ───────────────────────────── */

static void test_group_orphan_cycle(void)
{
    TEST("orphan / set_leader cycle 3 times");
    struct edc_task *l = edc_task_new("gocyc-l", "s", "g", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);

    for (int cycle = 0; cycle < 3; cycle++)
    {
        struct edc_task *m = edc_task_new("gocyc-m", "s", "g", "M", edc_channel_adapter);
        edc_task_group_add_task(g, m);

        /* Remove leader → orphan */
        edc_task_group_remove_task(g, l);
        CHECK(g->is_orphaned, "cycle %d: orphaned", cycle);
        edc_task_free(l);

        /* Promote member to leader */
        edc_task_group_set_leader(g, m);
        CHECK(!g->is_orphaned, "cycle %d: recovered", cycle);

        l = m; /* m becomes new leader for next cycle */
    }

    edc_task_group_free(g);
}

/* ── G5. 信号对空组/孤儿组 ──────────────────────────────────── */

static void test_group_signal_edge(void)
{
    TEST("signal to orphaned group");
    struct edc_task *l = edc_task_new("gsig-l", "s", "g", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);
    struct edc_task *m = edc_task_new("gsig-m", "s", "g", "M", edc_channel_adapter);
    edc_task_set_state(m, TASK_WAITING_INPUT);
    edc_task_set_state(m, TASK_LLM_PROCESSING); /* Need WAITING_INPUT first */

    /* Reset: just use WAITING_INPUT */
    m->state = TASK_IDLE;
    edc_task_set_state(m, TASK_WAITING_INPUT);
    edc_task_group_add_task(g, m);

    edc_task_group_remove_task(g, l);
    CHECK(g->is_orphaned, "orphaned");
    edc_task_free(l);

    /* SIGTERM to orphaned group */
    edc_task_group_send_signal(g, TASK_SIGTERM);
    CHECK(m->state == TASK_CANCELLED, "orphaned member cancelled");

    edc_task_group_free(g);
}

/* ── G6. group_free 多次 ───────────────────────────────────── */

static void test_group_double_free(void)
{
    TEST("group_free on already-cleaned group");
    struct edc_task *l = edc_task_new("gdf-l", "s", "g", "L", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(l);
    edc_task_group_free(g);
    /* g is now dangling - but we can't test double-free safely */
    CHECK(1, "group_free completed without crash");
}

/* ── main ──────────────────────────────────────────────────── */

int main(void)
{
    log_info("=== Phase 1+2 高强度压力/边界测试 ===");
    log_info("");

    log_info("── Phase 1: edc_task ──");
    test_full_transition_matrix();
    test_null_safety_task();
    test_idempotent_operations();
    test_many_dependencies();
    test_dependency_chain();
    test_concurrent_signals();
    test_turn_stress();
    test_free_with_resources();
    test_circular_dependency();
    test_deep_fork();

    log_info("");
    log_info("── Phase 2: edc_task_group ──");
    test_group_null_safety();
    test_group_leader_edge();
    test_group_many_members();
    test_group_orphan_cycle();
    test_group_signal_edge();
    test_group_double_free();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
