#ifndef __EDC_CLI_HISTORY_H__
#define __EDC_CLI_HISTORY_H__

/* ── CLI `!n` / `!!` history ─────────────────────────────────
 *
 * This is the SHELL'S OWN command history: its semantics (`!1` / `!!`, indices and
 * bounds) live entirely in edc_shell.c's edc_shell_expand_history(), and only the shell
 * reads it. One per process, not persisted, unrelated to session/channel.
 *
 * It is NOT the TUI's history; do not confuse it with it -- the input box's Up/Down
 * line recall is src/tui's own `history_` (a C++ member, transient, capped at 500;
 * THAT is "the frontend UI's history").
 *
 * (The sibling `edc_completion.c` used to be referenced here for contrast -- it held a
 * third, similar piece of state, the completion source's Frecency history. That module
 * was dead code and has been deleted; a completion system will be rebuilt separately.)
 *
 * Lineage: it used to hang on `struct edc_app` (so the shell's `!` expansion had to take
 * an umbrella and tests had to build an app for it), moved to assembly/ (today app/) in
 * P4, and in 2026-09-16 was judged to belong to the shell -> moved into src/shell/.
 * DIRECTIONAL reason: the thing that will really WRITE it is the TUI (whoever reads the
 * line the user typed records it), and the tui -> shell edge already exists; putting it
 * in tui instead would make the shell reach up into tui's package and require a reverse
 * leaf target -- needing that hack is itself a sign of misplacement (tried once, see
 * docs/runtime-model.md §11).
 *
 * EXISTING DEVIATION KEPT AS-IS: nothing in the production path writes it (the only
 * caller is a test), so in real use `!n`/`!!` never expand to anything. The move changes
 * no behaviour. Another historical oddity kept too: `#define CLI_HISTORY_MAX 1000` was
 * defined but never read (the history has no cap) -- the macro was dropped with the move,
 * but the cap is STILL "none". */

/** Record a line (blank lines and a repeat of the previous line are ignored). */
extern void edc_cli_history_add(const char *line);

/** Number of recorded lines. */
extern int edc_cli_history_count(void);

/** Line |index| (0-based, so `!1` is at(0)). NULL when out of range. */
extern const char *edc_cli_history_at(int index);

/** Clear it (for test isolation: this history is process-level now). */
extern void edc_cli_history_clear(void);

#endif
