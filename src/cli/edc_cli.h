#ifndef __EDC_CLI_H__
#define __EDC_CLI_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── edgeclaw_cli: the same input box, without the screen ─────────────────────
 *
 * The argument IS the line you would have typed into the TUI. Nothing else:
 *
 *     edgeclaw_cli '/task new "abc"'
 *     edgeclaw_cli '+"test shell like"'
 *     edgeclaw_cli '"a" | "b"'
 *     edgeclaw_cli '!make -j8'
 *
 * All four go through exactly the same path a keystroke does -- same passes, same
 * classification, same command table, same shell grammar, same sessions and groups. There
 * is no second command-line vocabulary to learn and none to keep in sync: if a line works
 * in the TUI it works here, and if it does not, it fails the same way with the same
 * message.
 *
 * What a caller chooses here is only what to do with the OUTPUT (it already exists as
 * messages before the process decides to print them):
 *
 *   (default)   the final answer on stdout
 *   --json      every message this run produced, one JSON object per line
 *   --silent    nothing; the run is still recorded, so `/task output <id>` has it later
 *
 * Exit codes: 0 the input ran to the end, 1 a task it created failed, 2 usage or
 * configuration, 3 timed out.
 *
 * It is a separate BINARY on purpose: it links the app, not ftxui, so a container or a
 * cron job ships a frontend that cannot draw anything even by accident.
 */

/* Run one input line. Returns the process exit code. */
int edc_cli_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
