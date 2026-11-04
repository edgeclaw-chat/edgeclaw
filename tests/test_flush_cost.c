/* tests/test_flush_cost.c -- what a flush reports about the I/O it caused.
 *
 * `edc_state_store_flush_measured(session, sched, &cost)` is the measuring form of the
 * flush: it samples the pager's cumulative write counter and PRAGMA page_size before and
 * after the transaction and reports `(pages_after - pages_before) * page_size` plus the
 * number of rows changed. `edc_state_store_flush()` is the same call with `out == NULL`.
 *
 * Why a file of its own: every other test in the tree drives `edc_state_store_flush`, i.e.
 * the NULL form, so the measuring half had never run. It is not dead code -- the state
 * probe asks for it (`edc_session_save_state_tagged` passes &cost when
 * EDGECLAW_STATE_PROBE is set) -- but no test ever asked, which is why the counter read sat
 * at zero coverage while the feature it feeds is a documented diagnostic.
 *
 * Driving flush_measured DIRECTLY is deliberate and is not a shortcut around the probe: the
 * behaviour under test is "the flush measures when asked", not "the probe decides to ask".
 * It also avoids the trap that would have made a probe-based test fragile -- the probe
 * caches its enablement in a uv_once, so the environment variable has to be set before the
 * module is first touched anywhere in the process (and a test binary touches it early).
 *
 * What is asserted, and why each one is not just "it returned":
 *
 *   - io_bytes >= 0: a real number, not the -1 "not measurable" sentinel;
 *   - io_bytes is a whole number of pages: the unit is pages × page_size, so a value that
 *     is not divisible by the page size means the arithmetic, not the measurement, is wrong;
 *   - more work reports more bytes: without this a constant would pass, and a constant is
 *     exactly what a mis-wired counter produces;
 *   - rows changed is non-zero: the other half of the report, and it comes from a different
 *     source (sqlite3_total_changes) than the page counter, so both are checked.
 */

#include "edc_compat.h"
#include "test_ws.h"

#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>

/* Stub pulled in transitively by the shell/command layer. */
volatile sig_atomic_t _run_loop = 1;

static int test_passed = 0;
static int test_failed = 0;

#define TEST(name) printf("  TEST: %-60s ", name)
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); test_failed++; } \
    else { printf("ok\n"); test_passed++; } \
} while (0)

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* the test process does not outlive this */
}

/* A session with |n_members| tasks, each carrying a turn of |msgs_per_turn| messages, so a
 * flush has a controllable amount of work to do. Returns the session; the caller flushes. */
static struct edc_session *make_session(struct edc_config *cfg, struct edc_scheduler *sched, const char *id_tag,
                                        int n_members, int msgs_per_turn)
{
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id(id_tag), "FC");
    install_resolver(sched, s);

    struct edc_task *leader =
        edc_task_new(edc_test_id("fc-lead"), s->session_id, edc_test_id("fc-lead"), "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(s, g);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;

    for (int i = 0; i < n_members; i++)
    {
        char desc[64];
        snprintf(desc, sizeof(desc), "member %d", i);
        struct edc_task *t = edc_task_new(edc_test_id(desc), s->session_id, edc_test_id("fc-lead"), desc,
                                          edc_channel_adapter);
        edc_task_group_add_task(g, t);

        edc_scheduler_attach_input(sched, t, desc);
        struct edc_turn *turn = t->active_turn;
        for (int m = 0; m < msgs_per_turn; m++)
        {
            char body[128];
            snprintf(body, sizeof(body), "message %d/%d of %s", m, msgs_per_turn, desc);
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON_AddStringToObject(msg, "content", body);
            edc_turn_add_message(turn, msg);
        }
        edc_task_mark_completed(t);
        t->completion_seq = 2 + i;
    }
    return s;
}

/* The measuring form reports a real number, in whole pages, that grows with the work. */
static void test_measured_flush_reports_real_cost(void)
{
    TEST("a measured flush reports page-level bytes and a row count");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *small = make_session(cfg, sched, "fc-small", 1, 1);

    /* The struct starts at the sentinel values the function sets; a caller must be able to
     * tell "not measured" from "measured zero". */
    struct edc_state_flush_cost cost = {123, 456};
    CHECK(edc_state_store_flush_measured(small, sched, &cost), "the measured flush succeeded");

    CHECK(cost.io_bytes >= 0, "io_bytes is a measurement, not the -1 sentinel (got %lld)",
          (long long)cost.io_bytes);
    CHECK(cost.rows > 0, "rows changed is counted (got %lld)", (long long)cost.rows);

    CHECK(cost.io_bytes > 0, "the first flush of a new session wrote pages (got %lld)",
          (long long)cost.io_bytes);

    /* pages x page_size -- so the value has to be a whole number of pages. The page size is
     * asked of the store's own connection rather than assumed: hardcoding 4096 would be a
     * test that passes on this machine and fails on a build with another default. This is
     * also the assertion that catches arithmetic wired up in the wrong order (bytes per
     * page, or subtracting page_size instead of pages). */
    const int page_size = edc_state_store_pragma_int("page_size");
    CHECK(page_size > 0, "the store reports its page size (got %d)", page_size);
    CHECK(page_size > 0 && cost.io_bytes % page_size == 0,
          "io_bytes is a whole number of %d-byte pages: %lld is not", page_size,
          (long long)cost.io_bytes);

    /* More work must report more bytes -- otherwise a constant would pass everything above,
     * and a constant is exactly what a mis-wired counter produces.
     *
     * The workload has to be big enough, and that is measured rather than guessed: 12
     * members x 40 messages still reported the SAME 7 pages as a 1 x 1 session, because up
     * to that point the pager writes one fixed set of pages and the rest is still
     * accumulating in the -wal. Crossing the threshold takes a few hundred rows: 60 x 200
     * measures 9 pages and 200 x 500 measures 19. 60 x 200 is used here -- comfortably above
     * it, without making the test slow. */
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    struct edc_session *big = make_session(cfg, sched2, "fc-big", 60, 200);
    struct edc_state_flush_cost big_cost = {-1, -1};
    CHECK(edc_state_store_flush_measured(big, sched2, &big_cost), "the big measured flush succeeded");
    CHECK(big_cost.io_bytes > cost.io_bytes, "a bigger session reports more I/O (%lld vs %lld)",
          (long long)big_cost.io_bytes, (long long)cost.io_bytes);

    /* rows come from sqlite3_total_changes, a different source than the page counter. */
    CHECK(big_cost.rows > cost.rows, "and more changed rows (%lld vs %lld)", (long long)big_cost.rows,
          (long long)cost.rows);

    edc_session_free(small);
    edc_session_free(big);
    edc_scheduler_free(sched);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
}

/* The NULL form is the one production uses every save: it must still flush, and it must
 * leave the caller's struct alone (there is none). This is the "off means off" half -- the
 * comment in the header says the measurement costs two pragmas, so it must not run when
 * nobody asked. */
static void test_plain_flush_does_not_measure(void)
{
    TEST("the plain flush still works and measures nothing");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session *s = make_session(cfg, sched, "fc-plain", 2, 3);

    CHECK(edc_state_store_flush(s, sched), "the plain flush succeeded");
    /* Flushing twice is the ordinary case (a task settles, then the session does): the
     * second must not fail just because there is nothing new. */
    CHECK(edc_state_store_flush(s, sched), "a second plain flush also succeeds");

    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace();
    log_info("== test_flush_cost: what a flush reports about its own I/O ==");

    test_measured_flush_reports_real_cost();
    test_plain_flush_does_not_measure();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
