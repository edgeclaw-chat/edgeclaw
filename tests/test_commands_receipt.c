/* The task-creation receipt (edc_commands_task_receipt_text): the one line a user sees
 * after 派活 -- which task, which group, when it runs. It is a pure function of the task
 * (group + its own dep edges), so the shapes are asserted directly; no actor, no scheduler.
 *
 * The call sites (plain input / @ / pipes, `/task new`, `!cmd`) are what make it reach the
 * user; they are covered by the existing command/local-shell tests staying green. */
#include "edc_commands.h"
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
    printf("=== task-creation receipt tests ===\n\n");

    /* #3 leads its own (user) group -- the plain `@desc` shape. */
    struct edc_task *t3 = edc_task_new("3", "s1", "3", "do it", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t3);

    TEST("new group, nothing in front");
    sds r = edc_commands_task_receipt_text(t3);
    CHECK(eq(r, "→ task #3 · group #3 (new) · starts now"), r);
    sdsfree(r);

    /* #4 joins the existing group. */
    struct edc_task *t4 = edc_task_new("4", "s1", "3", "more", edc_channel_adapter);
    edc_task_group_add_task(g, t4);

    TEST("existing group, nothing in front (no (new))");
    r = edc_commands_task_receipt_text(t4);
    CHECK(eq(r, "→ task #4 · group #3 · starts now"), r);
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

    /* group_free frees its member tasks too. */
    edc_task_group_free(g);
    edc_task_group_free(sg);

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
