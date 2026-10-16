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
 *     Review the staged changes. Focus on {{1:-correctness and error handling}}.
 *
 * Substitution is TEXTUAL and uses `{{…}}`, NEVER `$`: a `$` in the body is copied
 * verbatim and belongs entirely to the input layer it is later executed by (where `$$`,
 * `$N`, `$LAST` and `$NAME` are TASK REFERENCES -- see tests/test_cmd_parse.c). Keeping
 * the two layers' sigils apart is the whole point of the brace syntax: with `$1` we would
 * have eaten a reference the input layer also understands.
 */

#include "sds.h"
#include "utarray.h"

struct edc_prompt
{
    sds name;          /* the file's base name (no `.md`) */
    sds description;   /* NULL when absent (front matter, else first non-empty line) */
    sds argument_hint; /* NULL when absent */
    sds body;          /* never empty: a template with an empty body is refused */
};

void edc_prompt_free(struct edc_prompt *prompt);

/* Load one template from |dir| by name. NULL when there is no readable
 * `<dir>/<name>.md`, when the front matter is malformed, or when the body is empty.
 *
 * |name| must be a bare file name: empty, dot-prefixed, or containing anything outside
 * [A-Za-z0-9._-] is refused, so `/prompt ../../etc/passwd` cannot leave |dir|. */
struct edc_prompt *edc_prompt_load(const char *dir, const char *name);

/* Every template in |dir| (only direct `*.md` children), sorted by name. A missing or
 * unreadable directory yields an empty array, not an error. UT_array of
 * `struct edc_prompt *`; free each with edc_prompt_free, then utarray_free. */
UT_array *edc_prompts_list(const char *dir);

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
