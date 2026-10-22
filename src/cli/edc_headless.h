#ifndef __EDC_HEADLESS_H__
#define __EDC_HEADLESS_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Non-interactive frontend (--print / --mode json) ────────────────────────
 *
 * The TUI is one frontend; this is the other. Both drive the SAME app: created through
 * edc_app_new(), fed through edc_app_send_user_message() and read through the channel
 * adapter, exactly as the TUI does. There is no second agent loop -- a headless mode that
 * re-implemented the turn cycle would drift from the interactive one, and that drift is
 * the reason this is a separate FRONTEND rather than a separate engine.
 *
 * It has no terminal: no ftxui, no alternate screen, no colour, and it never reads stdin
 * (so it is safe under a pipe or in cron). Results go to stdout; progress and errors go to
 * stderr, so `edgeclaw -p "…" > answer.md` yields a clean file.
 *
 * Exit codes (stable; a caller may branch on them):
 *   0  the task completed
 *   1  the task failed (ERROR / CANCELLED / INTERRUPTED)
 *   2  usage or configuration problem (bad flags, no provider configured)
 *   3  timed out before the task reached a terminal state
 */

typedef enum
{
    EDC_HEADLESS_NONE = 0,
    EDC_HEADLESS_PRINT, /* -p / --print: the answer alone on stdout */
    EDC_HEADLESS_JSON,  /* --mode json: one JSON object per line (JSONL) */
} edc_headless_mode;

/* Which mode this command line asks for. A cheap scan, done by main() BEFORE any terminal
 * setup, so a piped or cron invocation never initialises ftxui. */
edc_headless_mode edc_headless_parse(int argc, char **argv);

/* Run one prompt in the parsed mode. |sandbox_override| has the same meaning as for the
 * TUI (-1 = keep the persisted config value). Returns the process exit code above. */
int edc_headless_run(int argc, char **argv, int sandbox_override, edc_headless_mode mode);

#ifdef __cplusplus
}
#endif

#endif
