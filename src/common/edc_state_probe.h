#ifndef EDC_STATE_PROBE_H
#define EDC_STATE_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ── Stage-0 baseline instrumentation for session state persistence ──────────
 *
 * Measures edc_session_save_state() / edc_session_load_state() so the SQLite
 * migration (stage 0) has a baseline to
 * compare against. Without these numbers "P95 < 5 ms" is a claim, not a result.
 *
 * Why not log_debug: edc_log_init() registers the file callback at LOG_INFO in
 * every non-debug build, so log_debug() is dropped exactly in the build whose
 * timings we care about. A debug build would distort the measurement. So this is
 * an env-gated probe that works in release builds.
 *
 * Enable with EDGECLAW_STATE_PROBE=<path>: one CSV line per save/load, plus a
 * percentile summary written to <path>.summary at exit. Unset => every call is a
 * single bool test and returns.
 *
 * THE RULE FOR dur_ns: the timer covers the persistence call ONLY. Anything the
 * probe itself needs -- notably the second serialization that produces doc_bytes --
 * is done after the timer has been stopped. Measuring that work as part of the
 * flush is how the first baseline reported 0.32 ms for a 268-byte session.
 *
 * CSV columns:
 *   ts_ms,kind,tag,session,dur_ms,doc_bytes,io_bytes,rows,n_tasks,n_msgs,ok
 *
 * Design constraints:
 *  - Single writer in practice (the scheduler actor thread is the only one that
 *    touches the store); a mutex guards the counters anyway so tests that drive
 *    several schedulers cannot corrupt them.
 *  - No allocation when disabled.
 */

/* Resolve EDGECLAW_STATE_PROBE. Idempotent; called lazily by the probes. */
void edc_state_probe_init(void);

/* True when EDGECLAW_STATE_PROBE names a writable path. */
bool edc_state_probe_enabled(void);

/* One flush of one session.
 *
 *  dur_ns    wall time of the store call alone (the probe's own work excluded)
 *  doc_bytes strlen of the serialized snapshot: the O(history) size one flush covers
 *  io_bytes  bytes the pager wrote (WAL frames in WAL mode) -- the real write
 *            amplification, not a number derived from doc_bytes. -1 = not measurable.
 *  rows      rows inserted or deleted by the transaction (FK cascades included); -1 = unknown
 *  n_tasks / n_msgs  size of the tree at flush time
 */
void edc_state_probe_save(uint64_t dur_ns, size_t doc_bytes, int64_t io_bytes, int64_t rows,
                          int64_t n_tasks, int64_t n_msgs, bool ok, const char *session_id,
                          const char *tag);

/* One load ATTEMPT of a session -- attempts, not successes, so that "we activated 40
 * sessions and every one of them found nothing" stays visible.
 *
 *  dur_ns  wall time of the store call alone
 *  rows    rows the loader produced (messages + tasks): the O(history) side of the cost.
 *          ok == false with rows == 0 means the store held nothing for this session
 *          (a brand-new session) -- not a failure.
 *  n_tasks / n_msgs  what the loader produced
 */
void edc_state_probe_load(uint64_t dur_ns, int64_t rows, bool ok, int64_t n_tasks, int64_t n_msgs,
                          const char *session_id, const char *tag);

/* Write the percentile summary to <EDGECLAW_STATE_PROBE>.summary and stderr.
 * Safe to call when disabled (no-op); safe to call more than once. */
void edc_state_probe_summary(void);

/* Monotonic nanoseconds. Wraps uv_hrtime so callers do not need <uv.h>. */
uint64_t edc_state_probe_now_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* EDC_STATE_PROBE_H */
