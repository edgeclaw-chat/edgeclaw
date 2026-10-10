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
    CHECK(edc_task_is_ready(t), "initial state is IDLE");
    CHECK(t->is_group_leader == 0, "not group leader by default");
    CHECK(t->deps == NULL, "deps is NULL");
    CHECK(t->dependents == NULL, "dependents is NULL");
    CHECK(t->deps_total == 0, "deps_total is 0");
    CHECK(t->deps_completed == 0, "deps_completed is 0");
    CHECK(t->output == NULL, "output is NULL");
    CHECK(t->prompt == NULL, "prompt is NULL");
    CHECK(t->prompt_name == NULL, "prompt_name is NULL");
    CHECK(t->todos == NULL, "todos is NULL");
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
    CHECK(child->fork_point_completion_seq == 42, "fork_point_completion_seq set");
    CHECK(edc_task_is_ready(child), "forked task starts at READY");

    edc_task_free(parent);
    edc_task_free(child);
}

/* ── 3. 状态机 ─────────────────────────────────────────────── */

/* 3. State machine (intent-named): every phase's legal/illegal transitions,
 * terminal states and the two resume target groups. */

/* 造一个停在 (step, life) 的任务 —— 绕过状态机, 只为摆场景 (等价于落盘读回后的样子)。 */
static struct edc_task *park_at(const char *id, edc_task_step step, edc_task_life life)
{
    struct edc_task *t = edc_task_new(id, "sess-1", id, "sm test", edc_channel_adapter);
    edc_task_force_move(t, step, life);
    return t;
}

static void test_state_machine(void)
{
    struct edc_task *t = edc_task_new("task-sm", "sess-1", "task-sm", "sm test", edc_channel_adapter);
    CHECK(t->step == EDC_TASKSTEP_READY && t->life == EDC_LIFE_ALIVE, "a new task parks at READY+ALIVE");

    TEST("valid transitions from READY (parked with its input)");
    CHECK(edc_task_await_exec(t), "READY → AWAIT_EXEC (dispatch)");
    CHECK(t->step == EDC_TASKSTEP_AWAIT_EXEC && t->life == EDC_LIFE_ALIVE, "… lands on AWAIT_EXEC+ALIVE");
    edc_task_free(t);

    t = park_at("sm1", EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_waiting_deps(t), "READY → AWAIT_DEPS");
    CHECK(t->step == EDC_TASKSTEP_AWAIT_DEPS, "… lands on AWAIT_DEPS");
    edc_task_free(t);

    t = park_at("sm2", EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_cancelled(t), "READY → CANCELLED");
    edc_task_free(t);

    t = park_at("sm3", EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_suspended(t), "READY → suspended (created parked: /task new --suspended)");
    CHECK(t->step == EDC_TASKSTEP_READY && t->life == EDC_LIFE_SUSPENDED, "… keeps the step it was on");
    edc_task_free(t);

    t = park_at("sm3b", EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_failed(t), "READY → ERROR (驱动时就绪任务发起不了 I/O: 没端口/相位不允许)");
    CHECK(edc_task_has_failed(t), "… lands on ERROR");
    edc_task_free(t);

    TEST("invalid transitions from READY");
    t = park_at("sm4", EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    CHECK(!edc_task_mark_completed(t), "READY → COMPLETED rejected (没跑过不可能完成)");
    CHECK(!edc_task_await_tools(t), "READY → AWAIT_TOOLS rejected (must go via AWAIT_EXEC)");
    CHECK(!edc_task_move(t, EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_ALIVE), "READY → AWAIT_RESOURCE rejected");
    CHECK(!edc_task_mark_ready(t), "READY → READY rejected (self)");
    CHECK(t->step == EDC_TASKSTEP_READY && t->life == EDC_LIFE_ALIVE, "nothing moved after a refused transition");
    edc_task_free(t);

    TEST("transitions from WAITING_DEPS");
    t = park_at("sm5", EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_ready(t), "AWAIT_DEPS → READY (deps satisfied)");
    edc_task_free(t);
    t = park_at("sm6", EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_cancelled(t), "AWAIT_DEPS → CANCELLED");
    edc_task_free(t);
    t = park_at("sm7", EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_failed(t), "AWAIT_DEPS → ERROR");
    edc_task_free(t);
    t = park_at("sm8", EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE);
    CHECK(!edc_task_await_exec(t), "AWAIT_DEPS → AWAIT_EXEC rejected (must go through READY)");
    edc_task_free(t);

    TEST("transitions from AWAIT_EXEC");
    t = park_at("sm9", EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
    CHECK(edc_task_await_tools(t), "AWAIT_EXEC → AWAIT_TOOLS (tool calls came back)");
    edc_task_free(t);
    t = park_at("sm10", EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_completed(t), "AWAIT_EXEC → COMPLETED (final answer, no tools)");
    CHECK(!edc_task_await_exec(t), "COMPLETED → AWAIT_EXEC rejected (terminal)");
    edc_task_free(t);
    t = park_at("sm11", EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_failed(t), "AWAIT_EXEC → ERROR");
    edc_task_free(t);
    t = park_at("sm12", EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
    CHECK(!edc_task_mark_suspended(t), "AWAIT_EXEC → suspended rejected (nothing to park mid-LLM)");
    edc_task_free(t);

    TEST("transitions from AWAIT_TOOLS");
    t = park_at("sm13", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
    CHECK(edc_task_await_exec(t), "AWAIT_TOOLS → AWAIT_EXEC (chain continues)");
    edc_task_free(t);
    t = park_at("sm14", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_completed(t), "AWAIT_TOOLS → COMPLETED");
    edc_task_free(t);
    t = park_at("sm15", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_failed(t), "AWAIT_TOOLS → ERROR");
    edc_task_free(t);
    t = park_at("sm16", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_cancelled(t), "AWAIT_TOOLS → CANCELLED");
    edc_task_free(t);
    t = park_at("sm17", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_suspended(t), "AWAIT_TOOLS → suspended (Ctrl+Z mid-flight)");
    CHECK(t->step == EDC_TASKSTEP_AWAIT_TOOLS && t->life == EDC_LIFE_SUSPENDED, "… step kept, life suspended");
    CHECK(!edc_task_mark_suspended(t), "suspending twice rejected (self)");
    edc_task_free(t);

    TEST("resume targets from SUSPENDED");
    t = park_at("sm18", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED);
    CHECK(edc_task_mark_ready(t), "SUSPENDED → READY");
    edc_task_free(t);
    t = park_at("sm19", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED);
    CHECK(edc_task_mark_waiting_deps(t), "SUSPENDED → AWAIT_DEPS (re-decide: deps first)");
    edc_task_free(t);
    t = park_at("sm20", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED);
    CHECK(edc_task_await_exec(t), "SUSPENDED → AWAIT_EXEC (re-request LLM)");
    edc_task_free(t);
    t = park_at("sm21", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED);
    CHECK(edc_task_await_tools(t), "SUSPENDED → AWAIT_TOOLS (re-dispatch the tools)");
    edc_task_free(t);
    t = park_at("sm22", EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED);
    CHECK(edc_task_mark_cancelled(t), "SUSPENDED → CANCELLED");
    CHECK(!edc_task_mark_completed(t), "a suspended task cannot complete");
    edc_task_free(t);

    TEST("resume targets from ERROR");
    const edc_task_life error_life = EDC_LIFE_ERROR;
    t = park_at("sm23", EDC_TASKSTEP_DONE, error_life);
    CHECK(edc_task_mark_ready(t), "ERROR → READY");
    edc_task_free(t);
    t = park_at("sm24", EDC_TASKSTEP_DONE, error_life);
    CHECK(edc_task_mark_waiting_deps(t), "ERROR → AWAIT_DEPS");
    edc_task_free(t);
    t = park_at("sm25", EDC_TASKSTEP_DONE, error_life);
    CHECK(edc_task_await_exec(t), "ERROR → AWAIT_EXEC (retry the failed round)");
    edc_task_free(t);
    t = park_at("sm26", EDC_TASKSTEP_DONE, error_life);
    CHECK(edc_task_await_tools(t), "ERROR → AWAIT_TOOLS");
    edc_task_free(t);
    t = park_at("sm27", EDC_TASKSTEP_DONE, error_life);
    CHECK(edc_task_mark_cancelled(t), "ERROR → CANCELLED");
    CHECK(!edc_task_mark_completed(t), "an errored task cannot complete");
    CHECK(!edc_task_mark_suspended(t), "ERROR → suspended rejected");
    edc_task_free(t);

    TEST("CANCELLED can be resumed (by the state machine, same targets as SUSPENDED/ERROR)");
    t = park_at("sm28", EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED);
    CHECK(edc_task_mark_ready(t), "CANCELLED → READY");
    edc_task_free(t);
    t = park_at("sm28b", EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED);
    CHECK(edc_task_mark_waiting_deps(t), "CANCELLED → AWAIT_DEPS");
    edc_task_free(t);
    t = park_at("sm28c", EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED);
    CHECK(edc_task_await_exec(t), "CANCELLED → AWAIT_EXEC (continue the turn)");
    edc_task_free(t);
    t = park_at("sm28d", EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED);
    CHECK(edc_task_await_tools(t), "CANCELLED → AWAIT_TOOLS (re-dispatch the tools)");
    edc_task_free(t);
    t = park_at("sm29", EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED);
    CHECK(!edc_task_mark_cancelled(t), "CANCELLED → CANCELLED rejected (self)");
    CHECK(!edc_task_mark_completed(t), "CANCELLED → COMPLETED rejected");
    edc_task_free(t);

    TEST("COMPLETED is terminal");
    t = park_at("sm30", EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);
    CHECK(!edc_task_mark_ready(t) && !edc_task_await_exec(t) && !edc_task_await_tools(t), "no way out of COMPLETED");
    CHECK(!edc_task_mark_failed(t) && !edc_task_mark_cancelled(t) && !edc_task_mark_suspended(t),
          "… not even to another terminal");
    edc_task_free(t);
}

/* 4. Display vocabulary: (step, life) -> string. Frozen words, so all 6x5 = 30
 * combinations are pinned, including two malformed ones with no producing path. */

struct name_row
{
    edc_task_step step;
    edc_task_life life;
    const char *str;
};

static void test_display_names(void)
{
    TEST("display names: the frozen vocabulary, (step, life) → word");
    static const struct name_row rows[] = {
        {EDC_TASKSTEP_READY, EDC_LIFE_ALIVE, "IDLE"},
        {EDC_TASKSTEP_READY, EDC_LIFE_SUSPENDED, "SUSPENDED"},
        {EDC_TASKSTEP_READY, EDC_LIFE_COMPLETED, "COMPLETED"},
        {EDC_TASKSTEP_READY, EDC_LIFE_CANCELLED, "CANCELLED"},
        {EDC_TASKSTEP_READY, EDC_LIFE_ERROR, "ERROR"},
        {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE, "WAITING_DEPS"},
        {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_SUSPENDED, "SUSPENDED"},
        {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_COMPLETED, "COMPLETED"},
        {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_CANCELLED, "CANCELLED"},
        {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ERROR, "ERROR"},
        {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE, "LLM_PROCESSING"},
        {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_SUSPENDED, "SUSPENDED"},
        {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_COMPLETED, "COMPLETED"},
        {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_CANCELLED, "CANCELLED"},
        {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ERROR, "ERROR"},
        {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE, "TOOL_CALLING"},
        {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED, "SUSPENDED"},
        {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_COMPLETED, "COMPLETED"},
        {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_CANCELLED, "CANCELLED"},
        {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ERROR, "ERROR"},
        {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_ALIVE, "WAITING_DEPS"},
        {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_SUSPENDED, "SUSPENDED"},
        {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_COMPLETED, "COMPLETED"},
        {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_CANCELLED, "CANCELLED"},
        {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_ERROR, "ERROR"},
        {EDC_TASKSTEP_DONE, EDC_LIFE_ALIVE, "IDLE"},
        {EDC_TASKSTEP_DONE, EDC_LIFE_SUSPENDED, "SUSPENDED"},
        {EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED, "COMPLETED"},
        {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED, "CANCELLED"},
        {EDC_TASKSTEP_DONE, EDC_LIFE_ERROR, "ERROR"},
    };

    struct edc_task *t = edc_task_new("t-names", "s", "g", "x", edc_channel_adapter);
    bool saw_retired = false;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
    {
        const struct name_row *r = &rows[i];
        t->step = r->step;
        t->life = r->life;
        const char *got = edc_task_state_str(t);
        if (strcmp(got, r->str) != 0)
        {
            log_error("    name drift: (%s,%s) → '%s', expected '%s'", edc_task_step_name(r->step),
                      edc_task_life_name(r->life), got, r->str);
            test_failed++;
        }
        else
        {
            test_passed++;
        }
        if (strcmp(got, "WAITING_INPUT") == 0)
            saw_retired = true;
    }
    CHECK(!saw_retired, "`WAITING_INPUT` is retired: no (step, life) can produce that word");
    CHECK(strcmp(edc_task_state_str(NULL), "IDLE") == 0, "NULL → IDLE (旧读法的判空口径)");

    edc_task_free(t);
}

/* 4b. Read-side truth table for the (step, life) predicates. Each predicate carries
 * a life condition, since suspending does not move step; CANCELLED is resumable. */

struct read_side_row
{
    edc_task_step step;
    edc_task_life life;
    bool alive, ready, deps, completed, cancelled, failed, resumable, parked, running;
};

static void test_step_life_read_side(void)
{
    TEST("read predicates: one row per reachable (step, life)");
    /* 行序即断言序 —— 失败信息只报谓词名, 靠行序定位。 */
    static const struct read_side_row rows[] = {
        {EDC_TASKSTEP_READY, EDC_LIFE_ALIVE, true, true, false, false, false, false, false, true, false},
        {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE, true, false, true, false, false, false, false, true, false},
        {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE, true, false, false, false, false, false, false, false, true},
        {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE, true, false, false, false, false, false, false, false, true},
        {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_SUSPENDED, false, false, false, false, false, false, true, false, false},
        {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED, false, false, false, false, false, false, true, false, false},
        {EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED, false, false, false, true, false, false, false, false, false},
        {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED, false, false, false, false, true, false, true, false, false},
        {EDC_TASKSTEP_DONE, EDC_LIFE_ERROR, false, false, false, false, false, true, true, false, false},
    };

    struct edc_task *t = edc_task_new("t-readside", "s", "g", "x", edc_channel_adapter);
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
    {
        const struct read_side_row *r = &rows[i];
        t->step = r->step;
        t->life = r->life;
        CHECK(edc_task_is_alive(t) == r->alive, "row: is_alive");
        CHECK(edc_task_is_ready(t) == r->ready, "row: is_ready");
        CHECK(edc_task_awaits_deps(t) == r->deps, "row: awaits_deps");
        CHECK(edc_task_is_completed(t) == r->completed, "row: is_completed");
        CHECK(edc_task_is_cancelled(t) == r->cancelled, "row: is_cancelled");
        CHECK(edc_task_has_failed(t) == r->failed, "row: has_failed");
        CHECK(edc_task_is_resumable(t) == r->resumable, "row: is_resumable");
        CHECK(edc_task_is_parked(t) == r->parked, "row: is_parked");
        CHECK(edc_task_is_running(t) == r->running, "row: is_running");
    }

    /* 两个**表外**的畸形组合: 没有任何路径产出它们, 但读侧不许崩, 而且谓词在那里
     * 必须说真话 (显示名另有一说: DONE+ALIVE 报 IDLE, AWAIT_RESOURCE+ALIVE 报
     * WAITING_DEPS —— 那是旧词汇表达能力不足留下的兜底, 见 4 的表)。 */
    t->step = EDC_TASKSTEP_DONE;
    t->life = EDC_LIFE_ALIVE;
    CHECK(!edc_task_is_ready(t), "DONE+ALIVE: not ready (the step says DONE)");
    t->step = EDC_TASKSTEP_AWAIT_RESOURCE;
    t->life = EDC_LIFE_ALIVE;
    CHECK(!edc_task_awaits_deps(t), "AWAIT_RESOURCE+ALIVE: does not await deps");
    CHECK(edc_task_is_alive(t), "AWAIT_RESOURCE+ALIVE: alive, though nothing renders it as such");

    /* NULL 安全: 谓词在空指针上一律 false。 */
    CHECK(!edc_task_is_alive(NULL) && !edc_task_is_ready(NULL) && !edc_task_awaits_deps(NULL), "NULL → false");
    CHECK(!edc_task_is_completed(NULL) && !edc_task_is_cancelled(NULL) && !edc_task_has_failed(NULL),
          "NULL → false (2)");
    CHECK(!edc_task_is_resumable(NULL) && !edc_task_is_parked(NULL) && !edc_task_is_running(NULL), "NULL → false (3)");

    edc_task_free(t);
}

/* 4c. Transition table as a declarative specification, checked cell by cell
 * (6x5 sources x 6x5 targets = 900). Self-transitions are omitted; the edge count is
 * pinned so the spec cannot be silently emptied. */

#define EDC_TABLE_EDGES 115

struct table_target
{
    edc_task_step step;
    edc_task_life life;
};

struct table_row
{
    edc_task_step step;
    edc_task_life life;
    size_t n;
    struct table_target to[5]; /* 最多 5 个目标 (挂起/失败的重启组) */
};

static const struct table_row g_table[] = {
    /* READY + ALIVE */
    {EDC_TASKSTEP_READY, EDC_LIFE_ALIVE, 5,
     {{EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED},
      {EDC_TASKSTEP_DONE, EDC_LIFE_ERROR},
      {EDC_TASKSTEP_READY, EDC_LIFE_SUSPENDED}}},
    /* READY + SUSPENDED */
    {EDC_TASKSTEP_READY, EDC_LIFE_SUSPENDED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* READY + COMPLETED */
    /* 没有出口 (n=0; 那个占位元素不参与匹配) */
    {EDC_TASKSTEP_READY, EDC_LIFE_COMPLETED, 0, {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE}}},
    /* READY + CANCELLED */
    {EDC_TASKSTEP_READY, EDC_LIFE_CANCELLED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* READY + ERROR */
    {EDC_TASKSTEP_READY, EDC_LIFE_ERROR, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_DEPS + ALIVE */
    {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE, 3,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED},
      {EDC_TASKSTEP_DONE, EDC_LIFE_ERROR}}},
    /* AWAIT_DEPS + SUSPENDED */
    {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_SUSPENDED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_DEPS + COMPLETED */
    /* 没有出口 (n=0; 那个占位元素不参与匹配) */
    {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_COMPLETED, 0, {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE}}},
    /* AWAIT_DEPS + CANCELLED */
    {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_CANCELLED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_DEPS + ERROR */
    {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ERROR, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_EXEC + ALIVE */
    {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE, 5,
     {{EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED},
      {EDC_TASKSTEP_DONE, EDC_LIFE_ERROR}}},
    /* AWAIT_EXEC + SUSPENDED */
    {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_SUSPENDED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_EXEC + COMPLETED */
    /* 没有出口 (n=0; 那个占位元素不参与匹配) */
    {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_COMPLETED, 0, {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE}}},
    /* AWAIT_EXEC + CANCELLED */
    {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_CANCELLED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_EXEC + ERROR */
    {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ERROR, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_TOOLS + ALIVE */
    {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE, 6,
     {{EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED},
      {EDC_TASKSTEP_DONE, EDC_LIFE_ERROR},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED}}},
    /* AWAIT_TOOLS + SUSPENDED */
    {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_SUSPENDED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_TOOLS + COMPLETED */
    /* 没有出口 (n=0; 那个占位元素不参与匹配) */
    {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_COMPLETED, 0, {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE}}},
    /* AWAIT_TOOLS + CANCELLED */
    {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_CANCELLED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_TOOLS + ERROR */
    {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ERROR, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_RESOURCE + ALIVE */
    {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_ALIVE, 3,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED},
      {EDC_TASKSTEP_DONE, EDC_LIFE_ERROR}}},
    /* AWAIT_RESOURCE + SUSPENDED */
    {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_SUSPENDED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_RESOURCE + COMPLETED */
    /* 没有出口 (n=0; 那个占位元素不参与匹配) */
    {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_COMPLETED, 0, {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE}}},
    /* AWAIT_RESOURCE + CANCELLED */
    {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_CANCELLED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* AWAIT_RESOURCE + ERROR */
    {EDC_TASKSTEP_AWAIT_RESOURCE, EDC_LIFE_ERROR, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* DONE + ALIVE */
    {EDC_TASKSTEP_DONE, EDC_LIFE_ALIVE, 4,
     {{EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED},
      {EDC_TASKSTEP_DONE, EDC_LIFE_SUSPENDED}}},
    /* DONE + SUSPENDED */
    {EDC_TASKSTEP_DONE, EDC_LIFE_SUSPENDED, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}},
    /* DONE + COMPLETED */
    /* 没有出口 (n=0; 那个占位元素不参与匹配) */
    {EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED, 0, {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE}}},
    /* DONE + CANCELLED */
    {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED, 4,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE}}},
    /* DONE + ERROR */
    {EDC_TASKSTEP_DONE, EDC_LIFE_ERROR, 5,
     {{EDC_TASKSTEP_READY, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_DEPS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE},
      {EDC_TASKSTEP_DONE, EDC_LIFE_CANCELLED}}}};

static const struct table_target *table_target_of(const struct table_row *row, edc_task_step step, edc_task_life life)
{
    for (size_t i = 0; i < row->n; i++)
    {
        if (row->to[i].step == step && row->to[i].life == life)
            return &row->to[i];
    }
    return NULL;
}

static void test_transition_table_spec(void)
{
    TEST("the (step, life) table matches its written-out spec");
    static const edc_task_step steps[] = {EDC_TASKSTEP_READY,          EDC_TASKSTEP_AWAIT_DEPS,
                                          EDC_TASKSTEP_AWAIT_EXEC,     EDC_TASKSTEP_AWAIT_TOOLS,
                                          EDC_TASKSTEP_AWAIT_RESOURCE, EDC_TASKSTEP_DONE};
    static const edc_task_life lifes[] = {EDC_LIFE_ALIVE, EDC_LIFE_SUSPENDED, EDC_LIFE_COMPLETED, EDC_LIFE_CANCELLED,
                                          EDC_LIFE_ERROR};

    /* 规格必须**盖满** 30 个源: 一个不漏, 一个不重。 */
    CHECK(sizeof(g_table) / sizeof(g_table[0]) == 30, "the spec has 30 source rows");

    struct edc_task *t = edc_task_new("t-spec", "s", "g", "x", edc_channel_adapter);
    int combos = 0, drift = 0, edges = 0, missing = 0, dup = 0;
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
    {
        for (size_t j = 0; j < sizeof(lifes) / sizeof(lifes[0]); j++)
        {
            int hits = 0;
            for (size_t r = 0; r < sizeof(g_table) / sizeof(g_table[0]); r++)
            {
                const struct table_row *row = &g_table[r];
                if (row->step != steps[i] || row->life != lifes[j])
                    continue;
                hits++;
                edges += (int)row->n;
                /* 逐格对照: 这个源 × 全部 30 个目标。 */
                for (size_t a = 0; a < sizeof(steps) / sizeof(steps[0]); a++)
                {
                    for (size_t b = 0; b < sizeof(lifes) / sizeof(lifes[0]); b++)
                    {
                        edc_task_force_move(t, row->step, row->life);
                        bool got = edc_task_move(t, steps[a], lifes[b]);
                        bool want = table_target_of(row, steps[a], lifes[b]) != NULL;
                        combos++;
                        if (got != want)
                        {
                            drift++;
                            log_error("    table drift: (%s,%s) → (%s,%s): code=%d spec=%d",
                                      edc_task_step_name(row->step), edc_task_life_name(row->life),
                                      edc_task_step_name(steps[a]), edc_task_life_name(lifes[b]), (int)got, (int)want);
                        }
                    }
                }
            }
            if (hits == 0)
                missing++;
            if (hits > 1)
                dup++;
        }
    }
    CHECK(dup == 0, "every source appears exactly once in the spec");
    CHECK(missing == 0, "no source is missing from the spec");
    CHECK(combos == 900, "900 combinations walked (30 sources x 30 targets)");
    CHECK(edges == EDC_TABLE_EDGES, "the spec lists EDC_TABLE_EDGES allowed edges (it cannot be emptied)");
    CHECK(drift == 0, "no drift between the written-out spec and the code");
    edc_task_free(t);
}

/* ── 4d. 意图写入口: 每个意图的目标态与被拒的情形 ───────────── */

static void test_intent_writes(void)
{
    TEST("intent writes land on the right (step, life)");
    struct edc_task *t = edc_task_new("t-intent", "s", "g", "x", edc_channel_adapter);
    CHECK(t->step == EDC_TASKSTEP_READY && t->life == EDC_LIFE_ALIVE, "a new task starts READY+ALIVE");
    CHECK(edc_task_await_exec(t), "READY → await_exec");
    CHECK(t->step == EDC_TASKSTEP_AWAIT_EXEC && t->life == EDC_LIFE_ALIVE, "… lands on AWAIT_EXEC+ALIVE");
    CHECK(!edc_task_mark_suspended(t), "AWAIT_EXEC → suspend refused (nothing parks mid-LLM)");
    CHECK(edc_task_await_tools(t), "AWAIT_EXEC → await_tools");
    CHECK(t->step == EDC_TASKSTEP_AWAIT_TOOLS, "… lands on AWAIT_TOOLS");
    CHECK(edc_task_mark_suspended(t), "AWAIT_TOOLS → suspend (the one mid-flight park)");
    CHECK(t->life == EDC_LIFE_SUSPENDED && t->step == EDC_TASKSTEP_AWAIT_TOOLS,
          "suspend only moves life: the step it stopped on is kept");
    CHECK(!edc_task_mark_suspended(t), "suspending twice is a self-transition");
    CHECK(edc_task_await_tools(t), "resume back onto the tool chain");
    CHECK(t->life == EDC_LIFE_ALIVE, "… which also brings life back to ALIVE");
    CHECK(edc_task_mark_completed(t), "tool chain done → completed");
    CHECK(t->step == EDC_TASKSTEP_DONE && t->life == EDC_LIFE_COMPLETED, "… lands on DONE+COMPLETED");
    CHECK(!edc_task_mark_ready(t) && !edc_task_await_exec(t) && !edc_task_mark_failed(t), "COMPLETED is terminal");
    edc_task_free(t);

    /* 其余落点: 每条都从摆好的源态开始, 不靠上一条的连锁 (连锁会让失败点难以定位)。 */
    t = park_at("ti1", EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    CHECK(edc_task_mark_waiting_deps(t), "READY → waiting_deps");
    CHECK(t->step == EDC_TASKSTEP_AWAIT_DEPS && t->life == EDC_LIFE_ALIVE, "… lands on AWAIT_DEPS+ALIVE");
    CHECK(!edc_task_await_exec(t), "AWAIT_DEPS → await_exec refused (must go through READY)");
    CHECK(edc_task_mark_ready(t), "deps satisfied → back to READY");
    CHECK(!edc_task_mark_ready(t), "… and only once (self-transition refused)");
    CHECK(edc_task_mark_cancelled(t), "READY → cancelled");
    CHECK(t->step == EDC_TASKSTEP_DONE && t->life == EDC_LIFE_CANCELLED, "… lands on DONE+CANCELLED");
    CHECK(!edc_task_mark_completed(t), "CANCELLED cannot complete");
    CHECK(edc_task_mark_ready(t), "… but the state machine still lets CANCELLED resume to READY");
    edc_task_free(t);

    t = park_at("ti2", EDC_TASKSTEP_DONE, EDC_LIFE_ERROR);
    CHECK(edc_task_mark_ready(t), "ERROR → READY (resume)");
    edc_task_free(t);

    t = park_at("ti3", EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);
    CHECK(!edc_task_mark_failed(t) && !edc_task_mark_cancelled(t), "COMPLETED → another terminal refused");
    edc_task_free(t);

    CHECK(!edc_task_move(NULL, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE), "move(NULL) is refused");
    CHECK(!edc_task_await_exec(NULL) && !edc_task_mark_ready(NULL) && !edc_task_mark_suspended(NULL),
          "intents are NULL-safe");
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
    CHECK(edc_task_user_input(ut, NULL) == NULL, "no input -> NULL");

    struct edc_turn *t1 = edc_turn_new();
    edc_turn_add_user_message(t1, "hello");
    edc_task_add_turn(ut, t1);
    sds u1 = edc_task_user_input(ut, NULL);
    CHECK(u1 && strcmp(u1, "hello") == 0, "single user input");
    sdsfree(u1);

    struct edc_turn *t2 = edc_turn_new();
    edc_turn_add_user_message(t2, "world");
    edc_turn_add_assistant_message(t2, cJSON_Parse("{\"role\":\"assistant\",\"content\":\"ignored\"}"));
    edc_task_add_turn(ut, t2);
    sds u2 = edc_task_user_input(ut, NULL);
    CHECK(u2 && strcmp(u2, "hello\nworld") == 0, "inputs joined, assistant ignored");
    sdsfree(u2);

    struct edc_turn *ta = edc_turn_new();
    edc_turn_add_user_message(ta, "inflight");
    ut->active_turn = ta;
    sds u3 = edc_task_user_input(ut, NULL);
    CHECK(u3 && strcmp(u3, "hello\nworld\ninflight") == 0, "active_turn input included");
    sdsfree(u3);
    CHECK(edc_task_user_input(NULL, NULL) == NULL, "edc_task_user_input(NULL) is NULL");

    /* turn 持有消息: 新 turn 直接从其队列取 user 输入 (assistant 忽略) */
    struct edc_turn *t3 = edc_turn_new();
    edc_turn_add_user_message(t3, "flushed");
    cJSON *am3 = cJSON_CreateObject();
    cJSON_AddStringToObject(am3, "role", "assistant");
    cJSON_AddStringToObject(am3, "content", "ignored");
    edc_turn_add_assistant_message(t3, am3);
    edc_task_add_turn(ut, t3);
    sds u4 = edc_task_user_input(ut, NULL);
    CHECK(u4 && strcmp(u4, "hello\nworld\nflushed\ninflight") == 0,
          "turn-owned input recovered from turn queue");
    sdsfree(u4);
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
    CHECK(edc_task_awaits_deps(target), "target enters WAITING_DEPS (was IDLE)");

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

    CHECK(edc_task_is_ready(target2), "target2 starts IDLE");
    edc_task_add_dependency(target2, sourceA, TASK_DEP_JOIN);
    CHECK(edc_task_awaits_deps(target2), "enter WAITING_DEPS after 1st dep");
    CHECK(target2->deps_total == 1, "deps_total=1");
    CHECK(target2->deps_completed == 0, "deps_completed=0");

    edc_task_add_dependency(target2, sourceB, TASK_DEP_JOIN);
    CHECK(target2->deps_total == 2, "deps_total=2");

    /* Complete A — not enough yet */
    edc_task_on_dep_completed(target2, sourceA);
    CHECK(target2->deps_completed == 1, "deps_completed=1");
    CHECK(edc_task_awaits_deps(target2), "still WAITING_DEPS (B not done)");

    /* Complete B — all done */
    edc_task_on_dep_completed(target2, sourceB);
    CHECK(target2->deps_completed == 2, "deps_completed=2");
    CHECK(edc_task_is_ready(target2), "advanced to READY");

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
    CHECK(edc_task_is_ready(target), "advanced to READY (all deps done/cancelled)");
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
    CHECK(edc_task_awaits_deps(a) && edc_task_awaits_deps(b), "a,b waiting on deps");

    edc_task_send_signal(c, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(c), "c cancelled");
    CHECK(edc_task_is_cancelled(b), "b cascaded to CANCELLED (JOIN)");
    CHECK(edc_task_is_ready(a), "a released to READY (SEQ drop), not cancelled");
    CHECK(a->deps_total == 0, "a's dropped dep no longer counted (SEQUENCE 仍作废)");
    CHECK(b->deps_total == 1 && b->deps && b->deps->source == c, "b 对 c 的 JOIN 边留着");

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
    CHECK(edc_task_is_cancelled(source), "source is CANCELLED");
    /* JOIN 依赖不可满足 → 依赖者也随之取消 */
    CHECK(edc_task_is_cancelled(target), "JOIN dependent cascaded to CANCELLED");
    CHECK(target->deps_total == 1 && target->deps && target->deps->source == source,
          "dependent 的边留着 (取消不摘边; resume 它会回到等依赖)");

    edc_task_free(target);
    edc_task_free(source);

    TEST("SIGTERM on SEQUENCE source only drops the dependency");
    struct edc_task *starget = edc_task_new("task-sig5", "sess-1", "task-sig5", "seq target", edc_channel_adapter);
    struct edc_task *ssource = edc_task_new("task-sig6", "sess-1", "task-sig5", "seq source", edc_channel_adapter);

    edc_task_add_dependency(starget, ssource, TASK_DEP_SEQUENCE);
    edc_task_send_signal(ssource, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(ssource), "seq source CANCELLED");
    CHECK(edc_task_is_ready(starget), "SEQUENCE dependent released, not cancelled");
    CHECK(starget->deps_total == 0, "dropped dep no longer counted");

    edc_task_free(starget);
    edc_task_free(ssource);

    TEST("SIGKILL: does NOT notify dependents");
    struct edc_task *target2 = edc_task_new("task-sig3", "sess-2", "task-sig3", "sig test target2", edc_channel_adapter);
    struct edc_task *source2 = edc_task_new("task-sig4", "sess-2", "task-sig3", "sig test source2", edc_channel_adapter);

    edc_task_add_dependency(target2, source2, TASK_DEP_JOIN);
    CHECK(target2->deps_total == 1, "target2 waits for source2");

    edc_task_send_signal(source2, TASK_SIGKILL);
    CHECK(edc_task_is_cancelled(source2), "source2 is CANCELLED");
    /* SIGKILL: target's deps_total unchanged */
    CHECK(target2->deps_total == 1, "target2 deps_total UNCHANGED (SIGKILL)");
    CHECK(edc_task_awaits_deps(target2), "target2 still WAITING_DEPS");

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

/* ── 8b. LLM 用量累积 ──────────────────────────────────────── */

static void test_usage(void)
{
    TEST("usage accumulates; the conversation size is remembered, not summed");
    struct edc_task *t = edc_task_new("task-usage", "sess-1", "task-usage", "usage test", edc_channel_adapter);

    CHECK(t->tokens_prompt == 0 && t->tokens_completion == 0 && t->tokens_total == 0 &&
              t->tokens_cached == 0 && t->last_total_tokens == 0,
          "a fresh task has spent nothing");

    edc_task_add_usage(t, 100, 20, 120, 40);
    CHECK(t->tokens_prompt == 100 && t->tokens_completion == 20 && t->tokens_total == 120 &&
              t->tokens_cached == 40,
          "the first response is counted");
    CHECK(t->last_total_tokens == 120,
          "the conversation is that round's input + output (usage.total_tokens)");

    /* A tool round re-sends the grown context: the counters add up, the
     * conversation size is replaced. */
    edc_task_add_usage(t, 300, 50, 350, 200);
    CHECK(t->tokens_prompt == 400 && t->tokens_completion == 70 && t->tokens_total == 470 &&
              t->tokens_cached == 240,
          "the counters accumulate across rounds");
    CHECK(t->last_total_tokens == 350, "the conversation size is the *latest* round, not the running total");

    /* A provider that omits total_tokens: reconstruct it so the group's total
     * stays the sum of its members. */
    edc_task_add_usage(t, 10, 5, 0, 0);
    CHECK(t->tokens_total == 485 && t->last_total_tokens == 15, "a missing total is prompt + completion");

    /* A response that carries no usage at all must not erase the last known
     * size (the group would otherwise show an empty context after a hiccup). */
    const long long prompt_before = t->tokens_prompt;
    const long long ctx_before = t->last_total_tokens;
    edc_task_add_usage(t, 0, 0, 0, 0);
    CHECK(t->tokens_prompt == prompt_before && t->last_total_tokens == ctx_before,
          "a response with no usage changes nothing");

    /* A broken provider sending negative counts: clamp to 0, never subtract. */
    edc_task_add_usage(t, 10, -5, 0, -1);
    CHECK(t->tokens_prompt == 420 && t->tokens_completion == 75 && t->tokens_total == 495 &&
              t->tokens_cached == 240,
          "negative counts clamp to 0 instead of subtracting");
    CHECK(t->last_total_tokens == 10, "and the size follows the reconstructed total");

    edc_task_add_usage(NULL, 1, 1, 2, 0); /* must not crash */

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
    CHECK(edc_task_is_ready(sub), "sub starts READY");

    edc_task_add_dependency(main, sub, TASK_DEP_JOIN);
    CHECK(edc_task_awaits_deps(main), "main waits for sub");
    CHECK(main->deps_total == 1, "main deps_total=1");

    /* Sub does work and completes */
    sds result = sdsnew("sub completed successfully");
    edc_task_set_output(sub, result);
    edc_task_await_exec(sub);
    edc_task_mark_completed(sub);

    /* Notify main */
    edc_task_notify_dependents(sub);
    CHECK(main->deps_completed == 1, "main deps_completed=1");
    CHECK(edc_task_is_ready(main), "main advanced to READY");

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
    CHECK(edc_task_awaits_deps(target), "still waiting");

    /* C is cancelled */
    edc_task_on_dep_cancelled(target, c);
    CHECK(target->deps_total == 2, "deps_total=2 after C cancelled");
    CHECK(target->deps_completed == 1, "deps_completed still 1");

    /* B completes */
    sds out_b = sdsnew("result from B");
    edc_task_set_output(b, out_b);
    edc_task_notify_dependents(b);
    CHECK(target->deps_completed == 2, "deps_completed=2");
    CHECK(edc_task_is_ready(target), "all resolved → READY");

    /* Context should include A and B outputs */
    cJSON *msgs = edc_task_get_context_messages(target, NULL);
    CHECK(cJSON_GetArraySize(msgs) == 2, "2 dep output messages");

    cJSON_Delete(msgs);
    edc_task_free(target);
    edc_task_free(a);
    edc_task_free(b);
    edc_task_free(c);
}

/* Failure also propagates: an upstream failure means its output never arrives, so
 * the dependent cascades to ERROR (resumable).
 * Mutation: removing the failed-dependents notify must fail this. */
static void count_move(void *ctx, struct edc_task *moved)
{
    (void)moved;
    (*(int *)ctx)++;
}

static void test_dep_failure_propagates(void)
{
    TEST("scenario: 上游失败 → JOIN 依赖者级联失败 (递归), **边留着**");

    struct edc_task *a = edc_task_new("f-a", "sess-fail", "f-a", "A", edc_channel_adapter);
    struct edc_task *b = edc_task_new("f-b", "sess-fail", "f-b", "B waits for A", edc_channel_adapter);
    struct edc_task *c = edc_task_new("f-c", "sess-fail", "f-c", "C waits for B", edc_channel_adapter);
    edc_task_add_dependency(b, a, TASK_DEP_JOIN);
    edc_task_add_dependency(c, b, TASK_DEP_JOIN);
    CHECK(edc_task_awaits_deps(b) && edc_task_awaits_deps(c), "b/c 都在等");

    edc_task_await_exec(a);
    edc_task_mark_completed(a);
    CHECK(!edc_task_mark_failed(a), "已完成的任务不会失败 (终态 → 拒绝)");
    CHECK(edc_task_awaits_deps(b) && edc_task_awaits_deps(c), "没真失败 → 一点都不广播");
    edc_task_force_move(a, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE); /* 夹具: 摆回在途 */

    edc_task_await_exec(a);
    CHECK(edc_task_mark_failed(a), "A 失败 (executor 的出口)");
    CHECK(edc_task_has_failed(a), "A 进 ERROR");
    CHECK(edc_task_has_failed(b), "B 级联失败 (JOIN: 拿不到 A 的产出了)");
    CHECK(edc_task_has_failed(c), "C 跟着 B 一起失败 (递归)");
    /* 边**留着** (2026-10-10 裁决): 源是 ERROR/CANCELLED 都能 resume ⇒ 这条边只是暂时
     * 满足不了, 不是永远满足不了。摘了它, resume 依赖者就会"当作从来没依赖过"地跑下去,
     * 静默丢掉它本该等到的输入。 */
    CHECK(b->deps_total == 1 && b->deps && b->deps->source == a, "B 对 A 的边留着");
    CHECK(a->dependents != NULL && a->dependents->target == b, "A 那一侧也还挂着");
    CHECK(c->deps_total == 1 && c->deps && c->deps->source == b, "C 对 B 的边同样留着");
    CHECK(b->output != NULL && strstr(b->output, "f-a") != NULL, "B 的失败原因点到是哪个依赖 (f-a)");

    int moved = 0;
    edc_task_drain_moves(a, count_move, &moved);
    CHECK(moved == 1, "A 的账上是 B —— 债**按跳**记, 每跳记在直接发起者头上");
    moved = 0;
    edc_task_drain_moves(b, count_move, &moved);
    CHECK(moved == 1, "B 的账上是 C —— 所以清账必须递归下去才收得干净");

    CHECK(!edc_task_mark_failed(a), "再失败一次被拒 (终态) —— 幂等, 不重复广播");
    edc_task_free(a);
    edc_task_free(b);
    edc_task_free(c);

    TEST("scenario: 上游失败 → SEQUENCE 边作废放行 (只要顺序, 不要产出)");
    struct edc_task *s = edc_task_new("f-s", "sess-seq", "f-s", "S", edc_channel_adapter);
    struct edc_task *d = edc_task_new("f-d", "sess-seq", "f-d", "D runs after S", edc_channel_adapter);
    edc_task_add_dependency(d, s, TASK_DEP_SEQUENCE);
    CHECK(edc_task_awaits_deps(d), "D 等在顺序上");
    edc_task_await_exec(s);
    CHECK(edc_task_mark_failed(s), "S 失败");
    CHECK(edc_task_is_ready(d), "D 转 READY (与取消那条路逐字同构)");
    CHECK(d->deps_total == 0, "那条 SEQUENCE 边已作废");
    edc_task_free(s);
    edc_task_free(d);

    TEST("scenario: 失败不动任何边 (JOIN 与 SEQUENCE 都还在)");
    struct edc_task *x = edc_task_new("f-x", "sess-mix", "f-x", "X fails", edc_channel_adapter);
    struct edc_task *y = edc_task_new("f-y", "sess-mix", "f-y", "Y is alive", edc_channel_adapter);
    struct edc_task *z = edc_task_new("f-z", "sess-mix", "f-z", "Z waits for both", edc_channel_adapter);
    edc_task_add_dependency(z, x, TASK_DEP_JOIN);
    edc_task_add_dependency(z, y, TASK_DEP_SEQUENCE);
    CHECK(z->deps_total == 2, "Z 有两条依赖");
    edc_task_await_exec(x);
    CHECK(edc_task_mark_failed(x), "X 失败");
    CHECK(edc_task_has_failed(z), "Z 级联失败 (那条 JOIN 满足不了了)");
    CHECK(z->deps_total == 2, "两条边都还在 (JOIN 不摘、SEQUENCE 也没动)");
    {
        bool saw_x = false, saw_y = false;
        for (struct edc_task_dep *d = z->deps; d; d = d->next)
        {
            if (d->source == x)
                saw_x = true;
            if (d->source == y)
                saw_y = true;
        }
        CHECK(saw_x && saw_y, "边连着 X 与 Y 两个源");
    }
    CHECK(y->dependents && y->dependents->target == z, "Y 那一侧也还挂着");
    edc_task_free(x);
    edc_task_free(y);
    edc_task_free(z);
}

/* 新语义的"出路": 依赖者被放回"等依赖"之后, 源一旦真的产出, 那条边完成, 依赖者被**自动
 * 放行**(AWAIT_DEPS → READY, 之后由扫描驱动), 不需要人工再点一次。 */
static void test_dep_completion_releases(void)
{
    TEST("scenario: 源产出 (完成通知) ⇒ 那条边完成 ⇒ 等依赖者自动转 READY");

    struct edc_task *src = edc_task_new("rel-src", "sess-rel", "rel-src", "source", edc_channel_adapter);
    struct edc_task *dst = edc_task_new("rel-dst", "sess-rel", "rel-dst", "waits for src", edc_channel_adapter);
    edc_task_add_dependency(dst, src, TASK_DEP_JOIN);
    CHECK(edc_task_awaits_deps(dst), "dst 在等");
    CHECK(!edc_task_is_ready(dst), "还在等 ⇒ 不是 READY");

    edc_task_await_exec(src);
    edc_task_mark_completed(src);
    edc_task_notify_dependents(src); /* 宿主的完成路径做的正是这件事 */

    CHECK(!edc_task_has_unresolved_deps(dst), "边完成了");
    CHECK(edc_task_is_ready(dst), "dst 被自动放行 (AWAIT_DEPS → READY)");

    /* 反向: 源先取消 (边留着) ⇒ dst 取消; 之后源再完成, dst 的边同样满足 ⇒ 它也能被放行。 */
    struct edc_task *s2 = edc_task_new("rel-src2", "sess-rel", "rel-src2", "source 2", edc_channel_adapter);
    struct edc_task *d2 = edc_task_new("rel-dst2", "sess-rel", "rel-dst2", "waits for src2", edc_channel_adapter);
    edc_task_add_dependency(d2, s2, TASK_DEP_JOIN);
    edc_task_send_signal(s2, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(d2), "d2 级联取消");
    CHECK(d2->deps_total == 1, "边留着");
    edc_task_force_move(s2, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
    edc_task_await_exec(s2);
    edc_task_mark_completed(s2);
    edc_task_notify_dependents(s2);
    CHECK(!edc_task_has_unresolved_deps(d2), "源后来产出 ⇒ d2 的边也满足");
    CHECK(edc_task_is_ready(d2) || edc_task_is_cancelled(d2), "(相位随实现; 关键是边不再是未决)");

    edc_task_free(src);
    edc_task_free(dst);
    edc_task_free(s2);
    edc_task_free(d2);
}

/* ── main ──────────────────────────────────────────────────── */

int main(void)
{
    log_info("=== Phase 1: edc_task 模块测试 ===");
    log_info("");

    test_lifecycle();
    test_fork();
    test_state_machine();
    test_display_names();
    test_step_life_read_side();
    test_transition_table_spec();
    test_intent_writes();
    test_turns();
    test_dependencies();
    test_dep_cancelled();
    test_dep_cancel_cascade();
    test_signals();
    test_output();
    test_prompt();
    test_usage();
    test_context_messages();
    test_scenario_fork_join();
    test_scenario_multi_deps();
    test_dep_failure_propagates();
    test_dep_completion_releases();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
