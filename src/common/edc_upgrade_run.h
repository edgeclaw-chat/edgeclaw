#ifndef __EDC_UPGRADE_RUN_H__
#define __EDC_UPGRADE_RUN_H__

#include <stdbool.h>

#include <uv.h>

/* Perform the upgrade: fetch the installer for a pinned tag, run it, report the result.
 *
 * The work is split across two stages because each needs a different thread:
 *
 *   1. a worker thread fetches <tag>/tools/install.sh and writes it to a private temp file
 *      (a network round trip; blocking the loop for it would freeze the interface);
 *   2. the loop thread uv_spawn()s `sh <script> --tag <tag> --dir <dir>` and drains its
 *      output (libuv handles belong to the loop that owns them).
 *
 * Splitting it that way is not ceremony: the same split exists in the device tools, and
 * collapsing it means either blocking the UI during a download or spawning from a thread
 * that does not own the loop.
 *
 * |on_done| runs on the loop thread exactly once, with the installer's exit status and the
 * tail of its output. |ok| is false when the script could not be fetched, written or
 * spawned, or when the installer exited non-zero -- the caller only needs to know whether the
 * new binary is in place.
 */
typedef void (*edc_upgrade_done_fn)(bool ok, const char *detail, void *ud);

struct edc_upgrade_run;

/* Returns NULL when nothing was started (bad arguments, no thread, no handles). That is not
 * an error to report as a crash: the caller shows "cannot upgrade" and the manual command. */
struct edc_upgrade_run *edc_upgrade_run_start(uv_loop_t *loop, const char *cacert_path, const char *tag,
                                             const char *install_dir, int timeout_ms, edc_upgrade_done_fn on_done,
                                             void *ud);

/* Join the worker, kill the child if still running, close the handles. Must run before the
 * loop is destroyed. The struct frees itself once the last handle has closed. */
void edc_upgrade_run_stop(struct edc_upgrade_run *run);

#endif /* __EDC_UPGRADE_RUN_H__ */
