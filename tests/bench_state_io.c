/* bench_state_io: the measuring stick for the SQLite migration (stage 0).
 *
 * Drives the REAL code paths (edc_session_load_state / edc_session_save_state)
 * against a real workspace, so the numbers are the ones the product pays -- not a
 * microbenchmark of a reimplementation.
 *
 * It is not a cc_test on purpose: it needs a workspace with real session data, so
 * it would either be skipped or flaky in CI. Run it by hand:
 *
 *   EDGECLAW_STATE_PROBE=/tmp/probe/s13.csv \
 *     bazel run //tests:bench_state_io -- <workspace> <session_id> <iterations>
 *
 * Two questions it answers, both of which the design doc needs numbers for:
 *
 *  1. What does one full flush cost today, and how does that cost split between
 *     serializing, printing to a string, and writing the bytes? The split decides
 *     whether the "cold start < 300 ms" target is reachable at all: if most of the
 *     time is deserialize/normalise rather than JSON parsing, moving to SQLite does
 *     not remove it, and the target has to change.
 *
 *  2. Repeated saves of an UNCHANGED tree: that is the steady-state write
 *     amplification (today: a full rewrite every time). Stage 2 is measured against
 *     exactly this.
 *
 * Per-call detail lands in the EDGECLAW_STATE_PROBE CSV (see edc_state_probe.h);
 * this binary prints the coarse table and the stage split. */

#include "cJSON.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_log.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_probe.h"
#include "edc_task_group.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* Stub pulled in by the shell translation unit that kernel links against. */
volatile sig_atomic_t _run_loop = 1;

/* The scheduler reaches its store through this manager-shaped handle (the same
 * in-memory manager the unit tests use; see test_session_state_file.c). */
static void install_sessions(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm;
}

static double now_ms(void)
{
    return (double)uv_hrtime() / 1e6;
}

static long count_tasks(struct edc_session *s)
{
    long n = 0;
    struct edc_task_group *g;
    DL_FOREACH(s->groups, g)
        n += g->member_count;
    return n;
}

int main(int argc, char **argv)
{
    const char *ws = (argc > 1) ? argv[1] : NULL;
    const char *sid = (argc > 2) ? argv[2] : "s13";
    const int iters = (argc > 3) ? atoi(argv[3]) : 5;

    if (!ws)
    {
        fprintf(stderr,
                "usage: bench_state_io <workspace> [session_id] [iterations]\n"
                "   eg: bench_state_io /tmp/probe-home/.edgeclaw/workspace s13 5\n");
        return 2;
    }

    /* edc_log_init() makes the probe summary fire on edc_log_close(). */
    edc_log_init();

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);

    /* |sid| is the session to measure, named the way the UI and the directories spell it:
     * "s15". The database keys on the number, so the prefix is stripped here -- the bench
     * must open the REAL session, not a fixture id. */
    const char *sid_num_text = (sid[0] == 's' && sid[1] >= '0' && sid[1] <= '9') ? sid + 1 : sid;
    struct edc_session *s = edc_session_new(cfg, (int64_t)strtoll(sid_num_text, NULL, 10), sid);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);

    printf("workspace : %s\n", ws);
    printf("session   : %s\n", sid);
    printf("probe     : %s\n\n", edc_state_probe_enabled() ? "enabled" : "disabled (set EDGECLAW_STATE_PROBE)");

    /* ── 1. load ─────────────────────────────────────────────────────────── */
    const double t_load0 = now_ms();
    const bool loaded = edc_session_load_state(s, sched);
    const double load_ms = now_ms() - t_load0;

    printf("── load_state ─────────────────────────────────────────────\n");
    printf("  result      : %s\n", loaded ? "ok" : "FAILED (no file / corrupt)");
    printf("  wall        : %.1f ms\n", load_ms);
    printf("  tasks       : %ld\n", count_tasks(s));
    printf("  messages    : %zu\n", edc_session_get_message_count(s));

    if (!loaded)
    {
        printf("\nnothing to measure without a loadable session.\n");
        edc_state_probe_summary();
        edc_log_close();
        return 1;
    }

    /* ── 2. the flush itself ───────────────────────────────────────────────
     *
     * There is no "serialize" stage to time any more: flush binds the live structs straight
     * into SQLite (edc_state_store.c). What is still worth splitting out is how much of the
     * cost is the pager, which the probe reports as io_bytes -- so this stage just states
     * that and leaves the number to the full save below. */
    printf("\n── the flush, no intermediate document ────────────────────\n");
    printf("  (a flush binds the structs directly; the cost is the write, measured below)\n");

    /* ── 3. repeated full saves (steady state: nothing changed) ──────────── */
    printf("\n── %d x full save_state (unchanged tree) ───────────────────\n", iters);
    double best = 1e18, worst = 0.0, total = 0.0;
    int ok_count = 0;
    for (int i = 0; i < iters; i++)
    {
        const double t0 = now_ms();
        const bool ok = edc_session_save_state(s, sched);
        const double ms = now_ms() - t0;
        if (ok)
            ok_count++;
        total += ms;
        if (ms < best)
            best = ms;
        if (ms > worst)
            worst = ms;
        printf("  #%-2d %10.1f ms  %s\n", i + 1, ms, ok ? "ok" : "FAILED");
    }
    if (ok_count)
        printf("  min %.1f / mean %.1f / max %.1f ms\n", best, total / (double)iters, worst);

    /* No "snapshot size per flush" line any more: a flush no longer builds a document. The
     * number that still means something is io_bytes -- pages the pager actually wrote, which
     * the probe reports per save (see the CSV), and which is what the old line was trying to
     * approximate. */
    printf("\n── per tool boundary ──────────────────────────────────────\n");
    printf("  bytes written per flush : see the io_bytes column of the probe CSV\n");

    edc_state_probe_summary();
    edc_log_close();
    return 0;
}
