/* The task-creation receipt (edc_commands_task_receipt_text /
 * edc_commands_tasks_receipt_text): the one line a user sees after 派活 -- and only when
 * there is something the UI does not already show.
 *
 * It is a pure function of the task(s) (group + its own dep edges), so the shapes are
 * asserted directly; no actor, no scheduler. The call sites (plain input / @ / pipes,
 * `/task new`, `!cmd`) are what make it reach the user; they are covered by the existing
 * command/local-shell tests staying green.
 *
 * Two rules are pinned here:
 *   - a PLAIN foreground creation is silent (the sidebar row and the echoed input say it);
 *   - one input => ONE receipt (a `a | b | c` chain aggregates instead of three lines). */
#include "edc_commands.h"
#include "test_ws.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "sds.h"

#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0;

#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
    } while (0)

#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool eq(const char *got, const char *want)
{
    return got && strcmp(got, want) == 0;
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    printf("=== task-creation receipt tests ===\n\n");

    /* #3 leads its own group, which has exactly one member -- the plain `+desc` shape. */
    struct edc_task *t3 = edc_task_new("3", "s1", "3", "do it", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t3);

    TEST("plain new-group creation: silent");
    sds r = edc_commands_task_receipt_text(t3);
    CHECK(sdslen(r) == 0, "empty receipt");
    sdsfree(r);

    TEST("background in a fresh group keeps (new)");
    t3->is_background = true;
    r = edc_commands_task_receipt_text(t3);
    CHECK(eq(r, "→ task #3 · group #3 (new) · background"), r);
    t3->is_background = false;
    sdsfree(r);

    /* #4 joins the existing group. */
    struct edc_task *t4 = edc_task_new("4", "s1", "3", "more", edc_channel_adapter);
    edc_task_group_add_task(g, t4);

    TEST("plain join: silent");
    r = edc_commands_task_receipt_text(t4);
    CHECK(sdslen(r) == 0, "empty receipt");
    sdsfree(r);

    TEST("background");
    t4->is_background = true;
    r = edc_commands_task_receipt_text(t4);
    CHECK(eq(r, "→ task #4 · group #3 · background"), r);
    t4->is_background = false;
    sdsfree(r);

    TEST("queued behind (SEQUENCE dep)");
    edc_task_add_dependency(t4, t3, TASK_DEP_SEQUENCE);
    r = edc_commands_task_receipt_text(t4);
    CHECK(eq(r, "→ task #4 · group #3 · queued behind #3"), r);
    sdsfree(r);

    /* #9 waits on #4 and injects its output (a JOIN dep); SEQUENCE would lose to it,
     * as a `|` segment does. */
    struct edc_task *t9 = edc_task_new("9", "s1", "3", "merge", edc_channel_adapter);
    edc_task_group_add_task(g, t9);
    edc_task_add_dependency(t9, t4, TASK_DEP_JOIN);

    TEST("waits + injects (JOIN dep)");
    r = edc_commands_task_receipt_text(t9);
    CHECK(eq(r, "→ task #9 · group #3 · waits for #4 (outputs injected)"), r);
    sdsfree(r);

    TEST("two JOIN deps, insertion order");
    edc_task_add_dependency(t9, t3, TASK_DEP_JOIN);
    r = edc_commands_task_receipt_text(t9);
    CHECK(eq(r, "→ task #9 · group #3 · waits for #4, #3 (outputs injected)"), r);
    sdsfree(r);

    TEST("system task: silent");
    struct edc_task *st = edc_task_new("7", "s1", "7", "fold", edc_channel_adapter);
    struct edc_task_group *sg = edc_task_group_new(st);
    sg->kind = EDC_GROUP_KIND_SYSTEM;
    r = edc_commands_task_receipt_text(st);
    CHECK(sdslen(r) == 0, "empty receipt");
    sdsfree(r);

    TEST("NULL task: silent");
    r = edc_commands_task_receipt_text(NULL);
    CHECK(sdslen(r) == 0, "empty receipt");
    sdsfree(r);

    /* ── One input => one receipt ───────────────────────────── */

    /* Two genuinely plain tasks (no deps, foreground) in one fresh group. */
    struct edc_task *p1 = edc_task_new("21", "s1", "21", "one", edc_channel_adapter);
    struct edc_task_group *g3 = edc_task_group_new(p1);
    struct edc_task *p2 = edc_task_new("22", "s1", "21", "two", edc_channel_adapter);
    edc_task_group_add_task(g3, p2);
    struct edc_task *plain[2] = {p1, p2};
    TEST("a chain of plain creations: still silent");
    r = edc_commands_tasks_receipt_text(plain, 2);
    CHECK(sdslen(r) == 0, "empty receipt");
    sdsfree(r);

    struct edc_task *one_notable[2] = {t9, t3}; /* t9 waits (JOIN), t3 is plain */
    TEST("a chain with anything notable names them all");
    r = edc_commands_tasks_receipt_text(one_notable, 2);
    CHECK(eq(r, "→ 2 tasks #9, #3 · group #3"), r);
    sdsfree(r);

    TEST("single task delegates to the task receipt");
    r = edc_commands_tasks_receipt_text(&t9, 1);
    CHECK(eq(r, "→ task #9 · group #3 · waits for #4, #3 (outputs injected)"), r);
    sdsfree(r);

    t3->is_background = true;
    t4->is_background = true;
    struct edc_task *both_bg[2] = {t3, t4};
    TEST("all background -> one summarised line");
    r = edc_commands_tasks_receipt_text(both_bg, 2);
    CHECK(eq(r, "→ 2 tasks #3, #4 · group #3 · background"), r);
    sdsfree(r);
    t3->is_background = false;
    t4->is_background = false;

    /* A chain that spans groups drops the group field (there is no single one). */
    struct edc_task *t10 = edc_task_new("10", "s1", "10", "elsewhere", edc_channel_adapter);
    struct edc_task_group *g2 = edc_task_group_new(t10);
    t4->is_background = true;
    t10->is_background = true;
    struct edc_task *two_groups[2] = {t4, t10};
    TEST("two groups -> no group field");
    r = edc_commands_tasks_receipt_text(two_groups, 2);
    CHECK(eq(r, "→ 2 tasks #4, #10 · background"), r);
    sdsfree(r);
    t4->is_background = false;

    TEST("no tasks: silent");
    r = edc_commands_tasks_receipt_text(NULL, 0);
    CHECK(sdslen(r) == 0, "empty receipt");
    sdsfree(r);

    /* group_free frees its member tasks too. */
    edc_task_group_free(g);
    edc_task_group_free(sg);
    edc_task_group_free(g2);
    edc_task_group_free(g3);

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
