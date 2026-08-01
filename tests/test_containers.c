#include "src/edc_containers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("[%3d] %-55s ", tests_run, name); } while (0)
#define PASS()   do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(fmt, ...) do { printf("FAIL: " fmt "\n", ##__VA_ARGS__); } while (0)
#define ASSERT(cond, fmt, ...) if (!(cond)) { FAIL(fmt, ##__VA_ARGS__); return; } else do {} while (0)

/* ---- basic operations ---- */

static void test_init_destroy(void)
{
    TEST("init / destroy");
    EdcQueue q;
    edc_queue_init(&q);
    ASSERT(q.data == NULL, "data not NULL");
    ASSERT(q.head == 0 && q.tail == 0 && q.capacity == 0, "indices not zero");
    edc_queue_destroy(&q);
    ASSERT(q.data == NULL && q.head == 0 && q.tail == 0 && q.capacity == 0, "destroy did not reset");
    PASS();
}

static void test_push_pop_single(void)
{
    TEST("push / pop single element");
    EdcQueue q;
    edc_queue_init(&q);

    int val = 42;
    edc_queue_push(&q, &val);
    ASSERT(q.head == 0 && q.tail == 1 && q.capacity >= 1, "push did not advance: head=%zu tail=%zu cap=%zu", q.head, q.tail, q.capacity);

    void *p = edc_queue_pop(&q);
    ASSERT(p == &val, "pop returned wrong pointer");
    ASSERT(q.head == 1 && q.tail == 1, "pop did not advance head");

    p = edc_queue_pop(&q);
    ASSERT(p == NULL, "pop on empty should return NULL");

    edc_queue_destroy(&q);
    PASS();
}

static void test_push_pop_many(void)
{
    TEST("push / pop N elements (N=1000)");
    EdcQueue q;
    edc_queue_init(&q);

    int vals[1000];
    for (int i = 0; i < 1000; i++) {
        vals[i] = i;
        edc_queue_push(&q, &vals[i]);
    }
    ASSERT(edc_queue_length(&q) == 1000, "length != 1000, got %zu", edc_queue_length(&q));

    for (int i = 0; i < 1000; i++) {
        void *p = edc_queue_pop(&q);
        ASSERT(p == &vals[i], "pop[%d] returned wrong pointer", i);
    }
    ASSERT(edc_queue_length(&q) == 0, "length != 0 after drain");
    ASSERT(edc_queue_pop(&q) == NULL, "pop on drained queue not NULL");

    edc_queue_destroy(&q);
    PASS();
}

static void test_length(void)
{
    TEST("length tracking");
    EdcQueue q;
    edc_queue_init(&q);

    ASSERT(edc_queue_length(&q) == 0, "initial length != 0");

    int v;
    for (int i = 0; i < 5; i++) {
        edc_queue_push(&q, &v);
        ASSERT(edc_queue_length(&q) == (size_t)(i + 1), "length mismatch on push %d", i);
    }

    for (int i = 5; i > 0; i--) {
        ASSERT(edc_queue_length(&q) == (size_t)i, "length mismatch before pop, expected %d", i);
        edc_queue_pop(&q);
    }
    ASSERT(edc_queue_length(&q) == 0, "length != 0 after all pops");

    edc_queue_destroy(&q);
    PASS();
}

/* ---- the fix: capacity reuse after drain ---- */

static void test_capacity_reused_after_drain(void)
{
    TEST("capacity reused after full drain (no realloc)");
    EdcQueue q;
    edc_queue_init(&q);

    int v;
    for (int i = 0; i < 200; i++)
        edc_queue_push(&q, &v);
    for (int i = 0; i < 200; i++)
        edc_queue_pop(&q);

    ASSERT(q.head > 0, "head should have drifted");
    size_t cap_before = q.capacity;

    edc_queue_push(&q, &v);
    ASSERT(q.capacity == cap_before, "capacity grew: %zu -> %zu (should reuse)", cap_before, q.capacity);
    ASSERT(q.head == 0 && q.tail == 1, "indices not reset after drain+push: head=%zu tail=%zu", q.head, q.tail);

    edc_queue_destroy(&q);
    PASS();
}

static void test_capacity_bounded_under_cycles(void)
{
    TEST("capacity bounded under repeated fill/drain (1000 cycles)");
    EdcQueue q;
    edc_queue_init(&q);

    int v;
    size_t max_cap = 0;

    for (int cycle = 0; cycle < 1000; cycle++) {
        for (int i = 0; i < 7; i++)
            edc_queue_push(&q, &v);
        for (int i = 0; i < 7; i++)
            edc_queue_pop(&q);

        if (q.capacity > max_cap) max_cap = q.capacity;
    }

    ASSERT(max_cap <= 16, "capacity grew to %zu after 1000 cycles (expected <= 16)", max_cap);
    ASSERT(q.capacity == max_cap, "capacity changed at end: %zu", q.capacity);

    edc_queue_destroy(&q);
    PASS();
}

static void test_capacity_bounded_pipe_pattern(void)
{
    TEST("capacity bounded with pipe pattern (1 in, 1 out, 50000 cycles)");
    EdcQueue q;
    edc_queue_init(&q);

    int v;
    size_t max_cap = 0;

    for (int i = 0; i < 50000; i++) {
        edc_queue_push(&q, &v);
        void *p = edc_queue_pop(&q);
        if (!p) { FAIL("pop returned NULL at iteration %d", i); return; }

        if (q.capacity > max_cap) max_cap = q.capacity;
    }

    ASSERT(max_cap <= 8, "capacity grew to %zu after 50000 pipe (expected <= 8)", max_cap);

    edc_queue_destroy(&q);
    PASS();
}

static void test_capacity_bounded_partial_drain(void)
{
    TEST("capacity bounded with partial drain (burst 100, drain 90)");
    EdcQueue q;
    edc_queue_init(&q);

    int v;
    size_t max_cap = 0;
    size_t max_len = 0;

    for (int cycle = 0; cycle < 500; cycle++) {
        for (int i = 0; i < 100; i++)
            edc_queue_push(&q, &v);
        for (int i = 0; i < 90; i++)
            edc_queue_pop(&q);

        if (q.capacity > max_cap) max_cap = q.capacity;
        size_t len = edc_queue_length(&q);
        if (len > max_len) max_len = len;
    }

    ASSERT(max_cap <= max_len * 2, "capacity %zu > 2 * max_len %zu", max_cap, max_len);

    while (edc_queue_length(&q) > 0)
        edc_queue_pop(&q);

    edc_queue_push(&q, &v);
    ASSERT(q.head == 0 && q.tail == 1, "indices not reset: head=%zu tail=%zu", q.head, q.tail);

    edc_queue_destroy(&q);
    PASS();
}

/* ---- stress: massive push/pop ---- */

static void test_massive_throughput(void)
{
    TEST("massive throughput: 1M push (non-NULL) + 1M pop");
    EdcQueue q;
    edc_queue_init(&q);

    int *vals = malloc(1000000 * sizeof(int));
    if (!vals) { FAIL("malloc failed"); return; }

    for (int i = 0; i < 1000000; i++)
        vals[i] = i;

    for (int i = 0; i < 1000000; i++)
        edc_queue_push(&q, &vals[i]);

    ASSERT(edc_queue_length(&q) == 1000000, "length != 1M");

    for (int i = 0; i < 1000000; i++) {
        void *p = edc_queue_pop(&q);
        if (p != &vals[i]) { FAIL("wrong pointer at index %d", i); free(vals); return; }
    }
    ASSERT(edc_queue_length(&q) == 0, "not empty after 1M pop");

    free(vals);
    edc_queue_destroy(&q);
    PASS();
}

/* ---- stress: alternating push/pop patterns ---- */

static void test_alternating_burst(void)
{
    TEST("alternating burst pattern: stay ~100 elements");
    EdcQueue q;
    edc_queue_init(&q);

    int v;
    size_t max_cap = 0;
    size_t max_len = 0;

    for (int cycle = 0; cycle < 10000; cycle++) {
        int push_n = (cycle % 20) + 1;
        for (int i = 0; i < push_n; i++)
            edc_queue_push(&q, &v);

        int pop_n = (cycle % 15);
        for (int i = 0; i < pop_n; i++)
            edc_queue_pop(&q);

        size_t len = edc_queue_length(&q);
        if (len > max_len) max_len = len;
        if (q.capacity > max_cap) max_cap = q.capacity;
    }

    while (edc_queue_length(&q) > 0)
        edc_queue_pop(&q);

    ASSERT(edc_queue_length(&q) == 0, "final queue not empty");
    ASSERT(max_cap > 0, "capacity never allocated");
    printf("(max_len=%zu max_cap=%zu) ", max_len, max_cap);

    edc_queue_push(&q, &v);
    ASSERT(q.head == 0 && q.tail == 1, "indices not reset: head=%zu tail=%zu", q.head, q.tail);

    edc_queue_destroy(&q);
    PASS();
}

/* ---- edge cases ---- */

static void test_destroy_after_partial_drain(void)
{
    TEST("destroy after partial drain (residual elements)");
    EdcQueue q;
    edc_queue_init(&q);

    int v;
    for (int i = 0; i < 3; i++)
        edc_queue_push(&q, &v);
    edc_queue_pop(&q);
    edc_queue_pop(&q);
    edc_queue_destroy(&q);
    PASS();
}

static void test_push_pop_push(void)
{
    TEST("push-pop-push sequence (empty between pushes)");
    EdcQueue q;
    edc_queue_init(&q);

    int v = 1;
    edc_queue_push(&q, &v);
    edc_queue_pop(&q);
    edc_queue_push(&q, &v);

    ASSERT(q.head == 0 && q.tail == 1, "indices wrong: head=%zu tail=%zu", q.head, q.tail);

    void *p = edc_queue_pop(&q);
    ASSERT(p == &v, "pop wrong pointer");
    ASSERT(q.head == 1 && q.tail == 1, "final indices wrong");

    edc_queue_destroy(&q);
    PASS();
}

static void test_reinit_reuse(void)
{
    TEST("reinit and reuse after destroy");
    EdcQueue q;

    for (int r = 0; r < 5; r++) {
        edc_queue_init(&q);
        int v;
        for (int i = 0; i < 100; i++)
            edc_queue_push(&q, &v);
        for (int i = 0; i < 100; i++)
            edc_queue_pop(&q);
        edc_queue_destroy(&q);

        ASSERT(q.data == NULL && q.capacity == 0, "destroy did not zero out");
    }
    PASS();
}

/* ---- regression: fixed head/tail reset preserves data correctly ---- */

static void test_reset_indices_dont_lose_data_when_parial(void)
{
    TEST("partial queue not incorrectly reset");
    EdcQueue q;
    edc_queue_init(&q);

    int a = 1, b = 2;
    edc_queue_push(&q, &a);
    edc_queue_push(&q, &b);
    edc_queue_pop(&q);

    ASSERT(q.head == 1 && q.tail == 2, "indices wrong after partial drain");

    edc_queue_push(&q, &a);
    ASSERT(q.tail == 3, "tail should be 3 not %zu (partial queue should not reset)", q.tail);

    void *p1 = edc_queue_pop(&q);
    void *p2 = edc_queue_pop(&q);
    ASSERT(p1 == &b && p2 == &a, "wrong elements after push to partial queue");

    edc_queue_destroy(&q);
    PASS();
}

/* ---- stress: NULL data pointer ---- */

static void test_null_data(void)
{
    TEST("push / pop NULL data pointer");
    EdcQueue q;
    edc_queue_init(&q);

    edc_queue_push(&q, NULL);
    ASSERT(edc_queue_length(&q) == 1, "length != 1 after NULL push");
    void *p = edc_queue_pop(&q);
    ASSERT(p == NULL, "NULL pop returned non-NULL");
    ASSERT(edc_queue_length(&q) == 0, "not empty after NULL pop");

    edc_queue_destroy(&q);
    PASS();
}

int main(void)
{
    printf("=== EdcQueue Test Suite ===\n\n");

    test_init_destroy();
    test_push_pop_single();
    test_push_pop_many();
    test_length();

    printf("\n--- Fix Verification: capacity reuse after drain ---\n");
    test_capacity_reused_after_drain();
    test_capacity_bounded_under_cycles();
    test_capacity_bounded_pipe_pattern();
    test_capacity_bounded_partial_drain();

    printf("\n--- Stress Tests ---\n");
    test_massive_throughput();
    test_alternating_burst();

    printf("\n--- Edge Cases ---\n");
    test_destroy_after_partial_drain();
    test_push_pop_push();
    test_reinit_reuse();
    test_reset_indices_dont_lose_data_when_parial();
    test_null_data();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
