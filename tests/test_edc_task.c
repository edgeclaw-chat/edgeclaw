#include "edc_task.h"
#include "edc_turn.h"
#include "edc_config.h"
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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

/* ── 1. Task 生命周期 ──────────────────────────────────────── */

static void test_lifecycle(void)
{
    TEST("task_new / task_free");
    struct edc_task *t = edc_task_new("task-1", "sess-1", "task-1", "test task", edc_channel_adapter);
    CHECK(t != NULL, "task_new returns non-NULL");
    CHECK(strcmp(t->task_id, "task-1") == 0, "task_id set");
    CHECK(strcmp(t->session_id, "sess-1") == 0, "session_id set");
    CHECK(strcmp(t->group_id, "task-1") == 0, "group_id set");
    CHECK(strcmp(t->description, "test task") == 0, "description set");
    CHECK(t->state == TASK_IDLE, "initial state is IDLE");
    CHECK(t->is_group_leader == 0, "not group leader by default");
    CHECK(t->deps == NULL, "deps is NULL");
    CHECK(t->dependents == NULL, "dependents is NULL");
    CHECK(t->deps_total == 0, "deps_total is 0");
    CHECK(t->deps_completed == 0, "deps_completed is 0");
    CHECK(t->output == NULL, "output is NULL");
    CHECK(t->prompt == NULL, "prompt is NULL");
    CHECK(t->prompt_name == NULL, "prompt_name is NULL");
    CHECK(t->todolist == NULL, "todolist is NULL");
    CHECK(t->forked_from == NULL, "forked_from is NULL");
    edc_task_free(t);

    TEST("task_new with NULL description");
    t = edc_task_new("task-2", "sess-1", "task-1", NULL, edc_channel_adapter);
    CHECK(strcmp(t->description, "") == 0, "NULL desc becomes empty string");
    edc_task_free(t);

    TEST("task_free handles NULL");
    edc_task_free(NULL);
    CHECK(1, "edc_task_free(NULL) does not crash");
}

/* ── 2. Task Fork ──────────────────────────────────────────── */

static void test_fork(void)
{
    TEST("task_fork inherits parent session_id and group_id");
    struct edc_task *parent = edc_task_new("task-parent", "sess-2", "task-parent", "parent", edc_channel_adapter);
    struct edc_task *child = edc_task_fork(parent, "task-child", "child task", 42, edc_channel_adapter);

    CHECK(child != NULL, "fork returns non-NULL");
    CHECK(strcmp(child->session_id, "sess-2") == 0, "child inherits session_id");
    CHECK(strcmp(child->group_id, "task-parent") == 0, "child inherits group_id");
    CHECK(strcmp(child->task_id, "task-child") == 0, "child has own task_id");
    CHECK(strcmp(child->description, "child task") == 0, "child has own description");
    CHECK(child->forked_from == parent, "forked_from set to parent");
    CHECK(child->fork_point_msg_count == 42, "fork_point_msg_count set");
    CHECK(child->state == TASK_WAITING_INPUT, "forked task starts at WAITING_INPUT");

    edc_task_free(parent);
    edc_task_free(child);
}

/* ── 3. 状态机 ─────────────────────────────────────────────── */

static void test_state_machine(void)
{
    struct edc_task *t = edc_task_new("task-sm", "sess-1", "task-sm", "sm test", edc_channel_adapter);

    TEST("valid transitions from IDLE");
    CHECK(edc_task_set_state(t, TASK_WAITING_DEPS), "IDLE → WAITING_DEPS");
    CHECK(t->state == TASK_WAITING_DEPS, "state updated");

    /* Reset to IDLE via new task for next test */
    edc_task_free(t);
    t = edc_task_new("task-sm2", "sess-1", "task-sm2", "sm test", edc_channel_adapter);
    CHECK(edc_task_set_state(t, TASK_WAITING_INPUT), "IDLE → WAITING_INPUT");
    edc_task_free(t);

    t = edc_task_new("task-sm3", "sess-1", "task-sm3", "sm test", edc_channel_adapter);
    CHECK(edc_task_set_state(t, TASK_CANCELLED), "IDLE → CANCELLED");
    edc_task_free(t);

    TEST("invalid transitions from IDLE");
    t = edc_task_new("task-sm4", "sess-1", "task-sm4", "sm test", edc_channel_adapter);
    CHECK(!edc_task_set_state(t, TASK_COMPLETED), "IDLE → COMPLETED rejected");
    CHECK(!edc_task_set_state(t, TASK_ERROR), "IDLE → ERROR rejected");
    CHECK(!edc_task_set_state(t, TASK_LLM_PROCESSING), "IDLE → LLM_PROCESSING rejected");
    CHECK(!edc_task_set_state(t, TASK_TOOL_CALLING), "IDLE → TOOL_CALLING rejected");
    CHECK(t->state == TASK_IDLE, "state unchanged after invalid transition");
    edc_task_free(t);

    TEST("transitions from WAITING_DEPS");
    t = edc_task_new("task-sm5", "sess-1", "task-sm5", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_DEPS);
    CHECK(edc_task_set_state(t, TASK_WAITING_INPUT), "WAITING_DEPS → WAITING_INPUT");
    edc_task_set_state(t, TASK_WAITING_DEPS);
    CHECK(edc_task_set_state(t, TASK_CANCELLED), "WAITING_DEPS → CANCELLED");
    edc_task_free(t);

    TEST("transitions from WAITING_INPUT");
    t = edc_task_new("task-sm6", "sess-1", "task-sm6", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    CHECK(edc_task_set_state(t, TASK_LLM_PROCESSING), "WAITING_INPUT → LLM_PROCESSING");
    edc_task_set_state(t, TASK_WAITING_INPUT);
    CHECK(edc_task_set_state(t, TASK_CANCELLED), "WAITING_INPUT → CANCELLED");
    edc_task_free(t);

    TEST("transitions from LLM_PROCESSING");
    t = edc_task_new("task-sm7", "sess-1", "task-sm7", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    CHECK(edc_task_set_state(t, TASK_TOOL_CALLING), "LLM_PROCESSING → TOOL_CALLING");
    CHECK(edc_task_set_state(t, TASK_COMPLETED), "TOOL_CALLING → COMPLETED");
    CHECK(!edc_task_set_state(t, TASK_LLM_PROCESSING), "COMPLETED → LLM_PROCESSING rejected (terminal)");

    /* new task: LLM_PROCESSING → CANCELLED */
    edc_task_free(t);
    t = edc_task_new("task-sm7b", "sess-1", "task-sm7b", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    CHECK(edc_task_set_state(t, TASK_CANCELLED), "LLM_PROCESSING → CANCELLED");

    /* new task: LLM_PROCESSING → ERROR */
    edc_task_free(t);
    t = edc_task_new("task-sm7c", "sess-1", "task-sm7c", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    CHECK(edc_task_set_state(t, TASK_ERROR), "LLM_PROCESSING → ERROR");
    edc_task_free(t);

    TEST("transitions from TOOL_CALLING");
    t = edc_task_new("task-sm8", "sess-1", "task-sm8", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_TOOL_CALLING);
    CHECK(edc_task_set_state(t, TASK_LLM_PROCESSING), "TOOL_CALLING → LLM_PROCESSING (continue)");
    CHECK(edc_task_set_state(t, TASK_COMPLETED), "LLM_PROCESSING → COMPLETED via TOOL_CALLING");

    /* new task: TOOL_CALLING → CANCELLED */
    edc_task_free(t);
    t = edc_task_new("task-sm8b", "sess-1", "task-sm8b", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_TOOL_CALLING);
    CHECK(edc_task_set_state(t, TASK_CANCELLED), "TOOL_CALLING → CANCELLED");

    /* new task: TOOL_CALLING → ERROR */
    edc_task_free(t);
    t = edc_task_new("task-sm8c", "sess-1", "task-sm8c", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_TOOL_CALLING);
    CHECK(edc_task_set_state(t, TASK_ERROR), "TOOL_CALLING → ERROR");
    edc_task_free(t);

    TEST("COMPLETED is terminal");
    t = edc_task_new("task-sm9", "sess-1", "task-sm9", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_COMPLETED);
    CHECK(!edc_task_set_state(t, TASK_WAITING_INPUT), "COMPLETED → WAITING_INPUT rejected");
    CHECK(!edc_task_set_state(t, TASK_IDLE), "COMPLETED → IDLE rejected");
    CHECK(t->state == TASK_COMPLETED, "state remains COMPLETED");
    edc_task_free(t);

    TEST("CANCELLED can be resumed");
    t = edc_task_new("task-sm10", "sess-1", "task-sm10", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_CANCELLED);
    CHECK(edc_task_set_state(t, TASK_WAITING_INPUT), "CANCELLED → WAITING_INPUT (resume)");
    CHECK(!edc_task_set_state(t, TASK_IDLE), "CANCELLED → IDLE rejected");
    edc_task_free(t);

    TEST("ERROR can be resumed or cancelled");
    t = edc_task_new("task-sm11", "sess-1", "task-sm11", "sm test", edc_channel_adapter);
    edc_task_set_state(t, TASK_WAITING_INPUT);
    edc_task_set_state(t, TASK_LLM_PROCESSING);
    edc_task_set_state(t, TASK_ERROR);
    CHECK(edc_task_set_state(t, TASK_WAITING_INPUT), "ERROR → WAITING_INPUT (resume)");
    edc_task_set_state(t, TASK_ERROR);
    CHECK(edc_task_set_state(t, TASK_CANCELLED), "ERROR → CANCELLED");
    edc_task_free(t);
}

/* ── 4. 状态名映射 ─────────────────────────────────────────── */

static void test_state_names(void)
{
    TEST("state_name returns correct strings");
    CHECK(strcmp(edc_task_state_name(TASK_IDLE), "IDLE") == 0, "IDLE name");
    CHECK(strcmp(edc_task_state_name(TASK_WAITING_DEPS), "WAITING_DEPS") == 0, "WAITING_DEPS name");
    CHECK(strcmp(edc_task_state_name(TASK_WAITING_INPUT), "WAITING_INPUT") == 0, "WAITING_INPUT name");
    CHECK(strcmp(edc_task_state_name(TASK_LLM_PROCESSING), "LLM_PROCESSING") == 0, "LLM_PROCESSING name");
    CHECK(strcmp(edc_task_state_name(TASK_TOOL_CALLING), "TOOL_CALLING") == 0, "TOOL_CALLING name");
    CHECK(strcmp(edc_task_state_name(TASK_COMPLETED), "COMPLETED") == 0, "COMPLETED name");
    CHECK(strcmp(edc_task_state_name(TASK_CANCELLED), "CANCELLED") == 0, "CANCELLED name");
    CHECK(strcmp(edc_task_state_name(TASK_ERROR), "ERROR") == 0, "ERROR name");

    TEST("classify_state round-trip");
    CHECK(edc_task_classify_state("IDLE") == TASK_IDLE, "classify 'IDLE'");
    CHECK(edc_task_classify_state("waiting_deps") == TASK_WAITING_DEPS, "classify 'waiting_deps' (case insensitive)");
    CHECK(edc_task_classify_state("LLM_PROCESSING") == TASK_LLM_PROCESSING, "classify 'LLM_PROCESSING'");
    CHECK(edc_task_classify_state("COMPLETED") == TASK_COMPLETED, "classify 'COMPLETED'");
    CHECK(edc_task_classify_state(NULL) == TASK_IDLE, "classify NULL → IDLE");
    CHECK(edc_task_classify_state("nonexistent") == TASK_IDLE, "classify unknown → IDLE");
}

/* ── 5. Turn 管理 ──────────────────────────────────────────── */

static void test_turns(void)
{
    TEST("add_turn and commit_active_turn");
    struct edc_task *t = edc_task_new("task-turn", "sess-1", "task-turn", "turn test", edc_channel_adapter);

    /* Add turn via add_turn */
    struct edc_turn *turn1 = edc_turn_new();
    edc_task_add_turn(t, turn1);
    CHECK(edc_queue_length(&t->turns) == 1, "1 turn in queue after add_turn");

    struct edc_turn *turn2 = edc_turn_new();
    edc_task_add_turn(t, turn2);
    CHECK(edc_queue_length(&t->turns) == 2, "2 turns in queue");

    /* Active turn commit */
    struct edc_turn *turn3 = edc_turn_new();
    t->active_turn = turn3;
    CHECK(t->active_turn != NULL, "active_turn set");
    edc_task_commit_active_turn(t);
    CHECK(t->active_turn == NULL, "active_turn NULL after commit");
    CHECK(edc_queue_length(&t->turns) == 3, "3 turns after commit");

    /* commit when active_turn is NULL */
    edc_task_commit_active_turn(t);
    CHECK(edc_queue_length(&t->turns) == 3, "no change when active_turn is NULL");

    /* add_turn NULL safety */
    edc_task_add_turn(t, NULL);
    CHECK(edc_queue_length(&t->turns) == 3, "add_turn NULL does nothing");

    edc_task_free(t);

    /* edc_task_message_count: message total = sum over committed turns +
     * in-flight active_turn (contrast with turns, which counts rounds). */
    TEST("edc_task_message_count sums committed turns + active_turn");
    struct edc_task *mt = edc_task_new("task-msg", "sess-1", "task-msg", "msg test", edc_channel_adapter);
    CHECK(edc_task_message_count(mt) == 0, "empty task has 0 messages");

    struct edc_turn *a = edc_turn_new();
    edc_turn_add_user_message(a, "u1");
    edc_task_add_turn(mt, a);
    CHECK(edc_task_message_count(mt) == 1, "1 message after 1 committed turn");

    struct edc_turn *b = edc_turn_new();
    edc_turn_add_user_message(b, "u2");
    edc_turn_add_assistant_message(b, cJSON_Parse("{\"role\":\"assistant\",\"content\":\"ok\"}"));
    edc_task_add_turn(mt, b);
    CHECK(edc_queue_length(&mt->turns) == 2, "2 turns committed");
    CHECK(edc_task_message_count(mt) == 3, "3 messages across 2 turns");

    struct edc_turn *act = edc_turn_new();
    edc_turn_add_user_message(act, "u3");
    mt->active_turn = act;
    CHECK(edc_task_message_count(mt) == 4, "in-flight active_turn message counted");
    edc_task_commit_active_turn(mt);
    CHECK(edc_task_message_count(mt) == 4, "count stable after commit");

    CHECK(edc_task_message_count(NULL) == 0, "edc_task_message_count(NULL) is 0");
    edc_task_free(mt);

    /* edc_task_user_input: user messages across turns + active_turn. */
    TEST("edc_task_user_input collects user messages");
    struct edc_task *ut = edc_task_new("task-uin", "sess-1", "task-uin", "u test", edc_channel_adapter);
    CHECK(edc_task_user_input(ut) == NULL, "no input -> NULL");

    struct edc_turn *t1 = edc_turn_new();
    edc_turn_add_user_message(t1, "hello");
    edc_task_add_turn(ut, t1);
    sds u1 = edc_task_user_input(ut);
    CHECK(u1 && strcmp(u1, "hello") == 0, "single user input");
    sdsfree(u1);

    struct edc_turn *t2 = edc_turn_new();
    edc_turn_add_user_message(t2, "world");
    edc_turn_add_assistant_message(t2, cJSON_Parse("{\"role\":\"assistant\",\"content\":\"ignored\"}"));
    edc_task_add_turn(ut, t2);
    sds u2 = edc_task_user_input(ut);
    CHECK(u2 && strcmp(u2, "hello\nworld") == 0, "inputs joined, assistant ignored");
    sdsfree(u2);

    struct edc_turn *ta = edc_turn_new();
    edc_turn_add_user_message(ta, "inflight");
    ut->active_turn = ta;
    sds u3 = edc_task_user_input(ut);
    CHECK(u3 && strcmp(u3, "hello\nworld\ninflight") == 0, "active_turn input included");
    sdsfree(u3);
    CHECK(edc_task_user_input(NULL) == NULL, "edc_task_user_input(NULL) is NULL");
    edc_task_free(ut);
}

/* ── 6. 依赖管理 ───────────────────────────────────────────── */

static void test_dependencies(void)
{
    TEST("add_dependency basic");
    struct edc_task *target = edc_task_new("task-target", "sess-1", "task-target", "target", edc_channel_adapter);
    struct edc_task *source = edc_task_new("task-source", "sess-1", "task-target", "source", edc_channel_adapter);

    bool ok = edc_task_add_dependency(target, source, TASK_DEP_JOIN);
    CHECK(ok, "add_dependency returns true");
    CHECK(target->deps_total == 1, "target deps_total is 1");
    CHECK(source->dependents != NULL, "source has dependents");
    CHECK(target->state == TASK_WAITING_DEPS, "target enters WAITING_DEPS (was IDLE)");

    TEST("duplicate dependency rejected");
    ok = edc_task_add_dependency(target, source, TASK_DEP_JOIN);
    CHECK(!ok, "duplicate dependency returns false");
    CHECK(target->deps_total == 1, "deps_total unchanged");

    TEST("self-dependency rejected");
    ok = edc_task_add_dependency(target, target, TASK_DEP_JOIN);
    CHECK(!ok, "self-dependency returns false");

    TEST("transitive cycle rejected");
    {
        struct edc_task *a = edc_task_new("ta-cyc", "sess-1", "ta-cyc", "A", edc_channel_adapter);
        struct edc_task *b = edc_task_new("tb-cyc", "sess-1", "ta-cyc", "B", edc_channel_adapter);
        struct edc_task *c = edc_task_new("tc-cyc", "sess-1", "ta-cyc", "C", edc_channel_adapter);
        CHECK(edc_task_add_dependency(a, b, TASK_DEP_JOIN), "a→b");
        CHECK(edc_task_add_dependency(b, c, TASK_DEP_JOIN), "b→c");
        CHECK(!edc_task_add_dependency(c, a, TASK_DEP_JOIN), "c→a rejected (cycle)");
        edc_task_free(a);
        edc_task_free(b);
        edc_task_free(c);
    }

    TEST("dep_completed advances state when all deps done");
    struct edc_task *target2 = edc_task_new("task-target2", "sess-1", "task-target2", "target2", edc_channel_adapter);
    struct edc_task *sourceA = edc_task_new("task-srcA", "sess-1", "task-target2", "srcA", edc_channel_adapter);
    struct edc_task *sourceB = edc_task_new("task-srcB", "sess-1", "task-target2", "srcB", edc_channel_adapter);

    CHECK(target2->state == TASK_IDLE, "target2 starts IDLE");
    edc_task_add_dependency(target2, sourceA, TASK_DEP_JOIN);
    CHECK(target2->state == TASK_WAITING_DEPS, "enter WAITING_DEPS after 1st dep");
    CHECK(target2->deps_total == 1, "deps_total=1");
    CHECK(target2->deps_completed == 0, "deps_completed=0");

    edc_task_add_dependency(target2, sourceB, TASK_DEP_JOIN);
    CHECK(target2->deps_total == 2, "deps_total=2");

    /* Complete A — not enough yet */
    edc_task_on_dep_completed(target2, sourceA);
    CHECK(target2->deps_completed == 1, "deps_completed=1");
    CHECK(target2->state == TASK_WAITING_DEPS, "still WAITING_DEPS (B not done)");

    /* Complete B — all done */
    edc_task_on_dep_completed(target2, sourceB);
    CHECK(target2->deps_completed == 2, "deps_completed=2");
    CHECK(target2->state == TASK_WAITING_INPUT, "advanced to WAITING_INPUT");

    /* dependents still exist but are marked completed */
    CHECK(sourceA->dependents != NULL, "sourceA still has dependent record");
    CHECK(sourceA->dependents->completed, "sourceA dependent marked completed");
    CHECK(sourceB->dependents != NULL, "sourceB still has dependent record");
    CHECK(sourceB->dependents->completed, "sourceB dependent marked completed");

    edc_task_free(target);
    edc_task_free(source);
    edc_task_free(target2);
    edc_task_free(sourceA);
    edc_task_free(sourceB);
}

static void test_dep_cancelled(void)
{
    /* SEQUENCE (`;`) 语义: 源被取消只抹去这条依赖, 目标照常可运行。 */
    TEST("SEQUENCE dep cancelled: edge dropped, target stays runnable");
    struct edc_task *target = edc_task_new("task-tgt-c", "sess-1", "task-tgt-c", "target-c", edc_channel_adapter);
    struct edc_task *source = edc_task_new("task-src-c", "sess-1", "task-tgt-c", "source-c", edc_channel_adapter);

    edc_task_add_dependency(target, source, TASK_DEP_SEQUENCE);
    CHECK(target->deps_total == 1, "deps_total=1 before cancel");

    edc_task_on_dep_cancelled(target, source);
    CHECK(target->deps_total == 0, "deps_total=0 after cancel");
    CHECK(target->state == TASK_WAITING_INPUT, "advanced to WAITING_INPUT (all deps done/cancelled)");
    CHECK(source->dependents != NULL, "source still has dependent record");
    CHECK(source->dependents->cancelled, "source dependent marked cancelled");

    edc_task_free(target);
    edc_task_free(source);
}

static void test_dep_cancel_cascade(void)
{
    /* chain: a --SEQ--> b --JOIN--> c  (a 依赖 b, b 依赖 c)
     * 取消 c: b 是 JOIN 依赖者 → 级联取消; a 是 b 的 SEQ 依赖者 → 只抹边、保持可运行。 */
    TEST("JOIN cancel cascades, SEQUENCE cancel only drops the edge");
    struct edc_task *a = edc_task_new("cas-a", "sess-cas", "cas-a", "A", edc_channel_adapter);
    struct edc_task *b = edc_task_new("cas-b", "sess-cas", "cas-a", "B", edc_channel_adapter);
    struct edc_task *c = edc_task_new("cas-c", "sess-cas", "cas-a", "C", edc_channel_adapter);

    CHECK(edc_task_add_dependency(a, b, TASK_DEP_SEQUENCE), "a -SEQ-> b");
    CHECK(edc_task_add_dependency(b, c, TASK_DEP_JOIN), "b -JOIN-> c");
    CHECK(a->state == TASK_WAITING_DEPS && b->state == TASK_WAITING_DEPS, "a,b waiting on deps");

    edc_task_send_signal(c, TASK_SIGTERM);
    CHECK(c->state == TASK_CANCELLED, "c cancelled");
    CHECK(b->state == TASK_CANCELLED, "b cascaded to CANCELLED (JOIN)");
    CHECK(a->state == TASK_WAITING_INPUT, "a released to WAITING_INPUT (SEQ drop), not cancelled");
    CHECK(a->deps_total == 0, "a's dropped dep no longer counted");

    edc_task_free(a);
    edc_task_free(b);
    edc_task_free(c);
}

/* ── 7. 信号系统 ───────────────────────────────────────────── */

static void test_signals(void)
{
    TEST("SIGTERM on JOIN source cascades the cancel to its dependent");
    struct edc_task *target = edc_task_new("task-sig1", "sess-1", "task-sig1", "sig test target", edc_channel_adapter);
    struct edc_task *source = edc_task_new("task-sig2", "sess-1", "task-sig1", "sig test source", edc_channel_adapter);

    edc_task_add_dependency(target, source, TASK_DEP_JOIN);
    CHECK(target->deps_total == 1, "target waits for source");

    /* Send SIGTERM to source */
    edc_task_send_signal(source, TASK_SIGTERM);
    CHECK(source->cancelled_request == 1, "source cancelled_request set");
    CHECK(source->state == TASK_CANCELLED, "source is CANCELLED");
    /* JOIN 依赖不可满足 → 依赖者也随之取消 */
    CHECK(target->state == TASK_CANCELLED, "JOIN dependent cascaded to CANCELLED");
    CHECK(target->deps_total == 0, "dependent deps cleared by cascade");

    edc_task_free(target);
    edc_task_free(source);

    TEST("SIGTERM on SEQUENCE source only drops the dependency");
    struct edc_task *starget = edc_task_new("task-sig5", "sess-1", "task-sig5", "seq target", edc_channel_adapter);
    struct edc_task *ssource = edc_task_new("task-sig6", "sess-1", "task-sig5", "seq source", edc_channel_adapter);

    edc_task_add_dependency(starget, ssource, TASK_DEP_SEQUENCE);
    edc_task_send_signal(ssource, TASK_SIGTERM);
    CHECK(ssource->state == TASK_CANCELLED, "seq source CANCELLED");
    CHECK(starget->state == TASK_WAITING_INPUT, "SEQUENCE dependent released, not cancelled");
    CHECK(starget->deps_total == 0, "dropped dep no longer counted");

    edc_task_free(starget);
    edc_task_free(ssource);

    TEST("SIGKILL: does NOT notify dependents");
    struct edc_task *target2 = edc_task_new("task-sig3", "sess-2", "task-sig3", "sig test target2", edc_channel_adapter);
    struct edc_task *source2 = edc_task_new("task-sig4", "sess-2", "task-sig3", "sig test source2", edc_channel_adapter);

    edc_task_add_dependency(target2, source2, TASK_DEP_JOIN);
    CHECK(target2->deps_total == 1, "target2 waits for source2");

    edc_task_send_signal(source2, TASK_SIGKILL);
    CHECK(source2->state == TASK_CANCELLED, "source2 is CANCELLED");
    /* SIGKILL: target's deps_total unchanged */
    CHECK(target2->deps_total == 1, "target2 deps_total UNCHANGED (SIGKILL)");
    CHECK(target2->state == TASK_WAITING_DEPS, "target2 still WAITING_DEPS");

    /* SIGTERM to target2 now to clean up */
    edc_task_send_signal(target2, TASK_SIGTERM);
    edc_task_free(target2);
    edc_task_free(source2);
}

/* ── 8. 输出管理 ───────────────────────────────────────────── */

static void test_output(void)
{
    TEST("output set and get");
    struct edc_task *t = edc_task_new("task-out", "sess-1", "task-out", "output test", edc_channel_adapter);

    CHECK(edc_task_get_output(t) == NULL, "initial output is NULL");

    sds out = sdsnew("hello world");
    edc_task_set_output(t, out);
    CHECK(strcmp(edc_task_get_output(t), "hello world") == 0, "output set correctly");

    /* Overwrite */
    sds out2 = sdsnew("replaced");
    edc_task_set_output(t, out2);
    CHECK(strcmp(edc_task_get_output(t), "replaced") == 0, "output overwritten");

    /* set_output NULL safety */
    edc_task_set_output(NULL, sdsnew("x"));
    edc_task_free(t);
}

/* ── 9. Prompt 绑定 ────────────────────────────────────────── */

static void test_prompt(void)
{
    TEST("prompt set with name and text");
    struct edc_task *t = edc_task_new("task-prompt", "sess-1", "task-prompt", "prompt test", edc_channel_adapter);

    edc_task_set_prompt(t, "code-reviewer", "You are a code reviewer.");
    CHECK(strcmp(t->prompt_name, "code-reviewer") == 0, "prompt_name set");
    CHECK(strcmp(t->prompt, "You are a code reviewer.") == 0, "prompt set");

    /* Overwrite */
    edc_task_set_prompt(t, "tester", "You are a tester.");
    CHECK(strcmp(t->prompt_name, "tester") == 0, "prompt_name overwritten");
    CHECK(strcmp(t->prompt, "You are a tester.") == 0, "prompt overwritten");

    /* Clear with NULL */
    edc_task_set_prompt(t, NULL, NULL);
    CHECK(t->prompt_name == NULL, "prompt_name cleared");
    CHECK(t->prompt == NULL, "prompt cleared");

    /* set_prompt NULL safety */
    edc_task_set_prompt(NULL, "x", "y");
    edc_task_free(t);
}

/* ── 10. 上下文构建 ────────────────────────────────────────── */

static void test_context_messages(void)
{
    TEST("context with custom prompt");
    struct edc_task *t = edc_task_new("task-ctx", "sess-1", "task-ctx", "ctx test", edc_channel_adapter);
    edc_task_set_prompt(t, "test-role", "You are a test assistant.");

    cJSON *msgs = edc_task_get_context_messages(t, NULL);
    CHECK(cJSON_GetArraySize(msgs) >= 1, "at least 1 message");

    cJSON *sys = cJSON_GetArrayItem(msgs, 0);
    cJSON *role = cJSON_GetObjectItem(sys, "role");
    cJSON *content = cJSON_GetObjectItem(sys, "content");
    CHECK(role && strcmp(role->valuestring, "system") == 0, "first message is system");
    CHECK(content && strcmp(content->valuestring, "You are a test assistant.") == 0, "custom prompt used");

    cJSON_Delete(msgs);
    edc_task_free(t);

    TEST("context without prompt falls back to default");
    t = edc_task_new("task-ctx2", "sess-1", "task-ctx2", "ctx test 2", edc_channel_adapter);
    /* Pass NULL config → no default prompt either, just dep output (none) */
    msgs = edc_task_get_context_messages(t, NULL);
    CHECK(cJSON_GetArraySize(msgs) == 0, "no messages without prompt or config");
    cJSON_Delete(msgs);
    edc_task_free(t);

    TEST("context includes dependency outputs");
    struct edc_task *parent = edc_task_new("task-ctx-p", "sess-3", "task-ctx-p", "parent ctx", edc_channel_adapter);
    struct edc_task *child = edc_task_new("task-ctx-c", "sess-3", "task-ctx-p", "child ctx", edc_channel_adapter);

    sds child_out = sdsnew("Analysis: found 3 bugs");
    edc_task_set_output(child, child_out);
    edc_task_add_dependency(parent, child, TASK_DEP_JOIN);
    /* mark child as completed */
    edc_task_on_dep_completed(parent, child);

    msgs = edc_task_get_context_messages(parent, NULL);
    /* Should have 1 message: the child's output injected */
    CHECK(cJSON_GetArraySize(msgs) >= 1, "dep output generates message");
    cJSON *dep_msg = cJSON_GetArrayItem(msgs, 0);
    content = cJSON_GetObjectItem(dep_msg, "content");
    CHECK(content != NULL, "dep message has content");
    CHECK(strstr(content->valuestring, "child ctx") != NULL, "dep output contains child description");
    CHECK(strstr(content->valuestring, "3 bugs") != NULL, "dep output contains child output");

    cJSON_Delete(msgs);
    edc_task_free(parent);
    edc_task_free(child);
}

/* ── 11. 综合场景 ──────────────────────────────────────────── */

static void test_scenario_fork_join(void)
{
    TEST("scenario: fork + join + complete chain");
    /* Simulate: Task-Main forks Task-Sub, waits for it, Sub completes */
    struct edc_task *main = edc_task_new("task-main", "sess-scenario", "task-main", "main task", edc_channel_adapter);
    struct edc_task *sub = edc_task_fork(main, "task-sub", "sub task", 10, edc_channel_adapter);

    CHECK(sub->forked_from == main, "sub forked from main");
    CHECK(sub->state == TASK_WAITING_INPUT, "sub starts WAITING_INPUT");

    edc_task_add_dependency(main, sub, TASK_DEP_JOIN);
    CHECK(main->state == TASK_WAITING_DEPS, "main waits for sub");
    CHECK(main->deps_total == 1, "main deps_total=1");

    /* Sub does work and completes */
    sds result = sdsnew("sub completed successfully");
    edc_task_set_output(sub, result);
    edc_task_set_state(sub, TASK_LLM_PROCESSING);
    edc_task_set_state(sub, TASK_COMPLETED);

    /* Notify main */
    edc_task_notify_dependents(sub);
    CHECK(main->deps_completed == 1, "main deps_completed=1");
    CHECK(main->state == TASK_WAITING_INPUT, "main advanced to WAITING_INPUT");

    /* Verify main's context includes sub's output */
    cJSON *msgs = edc_task_get_context_messages(main, NULL);
    cJSON *dep_msg = cJSON_GetArrayItem(msgs, 0);
    cJSON *content = cJSON_GetObjectItem(dep_msg, "content");
    CHECK(strstr(content->valuestring, "sub completed successfully") != NULL,
          "main context includes sub output");

    cJSON_Delete(msgs);
    edc_task_free(main);
    edc_task_free(sub);
}

static void test_scenario_multi_deps(void)
{
    TEST("scenario: multiple dependencies with partial completion");
    struct edc_task *target = edc_task_new("task-tgt", "sess-multi", "task-tgt", "aggregator", edc_channel_adapter);
    struct edc_task *a = edc_task_new("task-a", "sess-multi", "task-tgt", "worker A", edc_channel_adapter);
    struct edc_task *b = edc_task_new("task-b", "sess-multi", "task-tgt", "worker B", edc_channel_adapter);
    struct edc_task *c = edc_task_new("task-c", "sess-multi", "task-tgt", "worker C", edc_channel_adapter);

    edc_task_add_dependency(target, a, TASK_DEP_SEQUENCE);
    edc_task_add_dependency(target, b, TASK_DEP_SEQUENCE);
    edc_task_add_dependency(target, c, TASK_DEP_SEQUENCE);
    CHECK(target->deps_total == 3, "target has 3 deps");

    /* A completes */
    sds out_a = sdsnew("result from A");
    edc_task_set_output(a, out_a);
    edc_task_notify_dependents(a);
    CHECK(target->deps_completed == 1, "deps_completed=1");
    CHECK(target->state == TASK_WAITING_DEPS, "still waiting");

    /* C is cancelled */
    edc_task_on_dep_cancelled(target, c);
    CHECK(target->deps_total == 2, "deps_total=2 after C cancelled");
    CHECK(target->deps_completed == 1, "deps_completed still 1");

    /* B completes */
    sds out_b = sdsnew("result from B");
    edc_task_set_output(b, out_b);
    edc_task_notify_dependents(b);
    CHECK(target->deps_completed == 2, "deps_completed=2");
    CHECK(target->state == TASK_WAITING_INPUT, "all resolved → WAITING_INPUT");

    /* Context should include A and B outputs */
    cJSON *msgs = edc_task_get_context_messages(target, NULL);
    CHECK(cJSON_GetArraySize(msgs) == 2, "2 dep output messages");

    cJSON_Delete(msgs);
    edc_task_free(target);
    edc_task_free(a);
    edc_task_free(b);
    edc_task_free(c);
}

/* ── main ──────────────────────────────────────────────────── */

int main(void)
{
    log_info("=== Phase 1: edc_task 模块测试 ===");
    log_info("");

    test_lifecycle();
    test_fork();
    test_state_machine();
    test_state_names();
    test_turns();
    test_dependencies();
    test_dep_cancelled();
    test_dep_cancel_cascade();
    test_signals();
    test_output();
    test_prompt();
    test_context_messages();
    test_scenario_fork_join();
    test_scenario_multi_deps();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
