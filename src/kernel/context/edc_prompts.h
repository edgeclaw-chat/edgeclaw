#ifndef __EDC_PROMPTS_H__
#define __EDC_PROMPTS_H__

/* ── Prompt library: `<workspace>/prompts/<name>.md` ─────────
 *
 * A prompt template is a reusable piece of TEXT, not a command. It is reachable only
 * as the ARGUMENT of the single `/prompt` built-in, so user content can never occupy a
 * name in the command namespace (the invariant in docs/prompt-library.md §4).
 *
 * File shape (front matter optional):
 *
 *     ---
 *     description: Review staged git changes
 *     argument-hint: "[focus]"
 *     ---
 *     Review the staged changes. Focus on ${1:-correctness and error handling}.
 *
 * Substitution is TEXTUAL, and the placeholders are THIS layer's: `$1` / `$@` /
 * `$ARGUMENTS` / `${1:-x}` -- the spelling pi / Claude Code / opencode use, so a template
 * brought over from them runs unedited. `{{…}}` is the legacy alias of the same forms and
 * keeps working. TASK REFERENCES ARE `{{…}}` TOO but live in the INPUT LAYER this text is
 * later executed by (`{{LAST}}` / `{{<id>}}` / `{{SELF}}`, see edc_shell_vars.c): a body
 * writes `{{LAST}}` for the previous output, and `{{1}}` means "argument 1" here because
 * this layer runs first.
 */

#include "sds.h"
#include "utarray.h"
#include <stdbool.h>

struct edc_prompt
{
    sds name;          /* the file's base name (no `.md`) */
    sds description;   /* NULL when absent (front matter, else first non-empty line) */
    sds argument_hint; /* NULL when absent */
    sds body;          /* never empty: a template with an empty body is refused */
    sds source;        /* the resource ROOT it came from ("" = the single-dir API) */
};

void edc_prompt_free(struct edc_prompt *prompt);

/* Load one template from |dir| by name. NULL when there is no readable
 * `<dir>/<name>.md`, when the front matter is malformed, or when the body is empty.
 *
 * |name| must be a bare file name: empty, dot-prefixed, or containing anything outside
 * [A-Za-z0-9._-] is refused, so `/prompt ../../etc/passwd` cannot leave |dir|.
 *
 * A body keeps its placeholders AS WRITTEN (`$1`, `{{1}}`, ...); edc_prompt_expand reads
 * both. */
struct edc_prompt *edc_prompt_load(const char *dir, const char *name);

/* Every template in |dir| (only direct `*.md` children), sorted by name. A missing or
 * unreadable directory yields an empty array, not an error. UT_array of
 * `struct edc_prompt *`; free each with edc_prompt_free, then utarray_free. */
UT_array *edc_prompts_list(const char *dir);

/* ── Over the resource roots (edc_resources.h) ───────────────
 *
 * A root may keep its templates in `prompts/` (pi) or `command/` `commands/` (opencode /
 * Claude Code); both layouts are read. Roots are searched IN ORDER and the first one that
 * has a name wins -- so the workspace shadows a migrated template of the same name, and
 * `p->source` says which root a template actually came from. */
struct edc_prompt *edc_prompt_load_roots(const UT_array *roots, const char *name);
UT_array *edc_prompts_list_roots(const UT_array *roots);

/* Split a command tail into arguments: whitespace-separated, quote-aware (`"…"` /
 * `'…'`), backslash escapes the next byte. Operators (`| ; & >`) are NOT special here --
 * they are ordinary bytes, because these are a template's inputs, not a shell line.
 * Returns a NULL-terminated-free array of sds plus *out_count; free with
 * edc_prompt_args_free. */
sds *edc_prompt_split_args(const char *s, int *out_count);
void edc_prompt_args_free(sds *args, int count);

/* Substitute the placeholders below in |prompt|'s body using |args| (|argc| of them).
 * A placeholder is `{{…}}`; recognised forms -- a missing positional argument yields the
 * EMPTY STRING, never a literal:
 *
 *   {{1}} … {{9}} … {{N}}   one positional argument (multi-digit: `{{10}}` is the tenth)
 *   {{@}}                   all arguments, space-joined
 *   {{N:-D}}                positional N, or D when it is missing/empty
 *   {{@:-D}}                all arguments, or D when there are none
 *   {{@:N}}                 arguments from N onward (1-based)
 *   {{@:N:L}}               L arguments from N onward
 *
 * An unrecognised `{{…}}` (including a lone `{{`) is copied VERBATIM, and so is every `$`
 * -- the template layer never reads `$`, so nothing it emits can be mistaken for one of
 * its own placeholders or stolen from the input layer. */
sds edc_prompt_expand(const struct edc_prompt *prompt, sds *args, int argc);

#endif /* __EDC_PROMPTS_H__ */
