#include "edc_shell_vars.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

/* ── Task references (the expand phase, called once deps are done) ──
 *
 * The sigil is `{{…}}`, NOT `$`: `$` belongs to the PROMPT TEMPLATE layer (`$1` … `$N` and
 * `$ARGUMENTS`). One sigil per layer, and they cannot collide. `$` is inert here -- it is
 * copied like any other byte, which is what keeps `$HOME`, `$1` and `$5.00` safe.
 *
 * Recognised forms -- and ONLY these; everything else is copied VERBATIM, because a
 * reference must be spelled exactly to count (that is what makes pasted text safe: a
 * Jinja `{{ var }}`, a stray brace, a `$HOME` all pass through untouched):
 *
 *     {{LAST}}      the last completed task's output
 *     {{<digits>}}  the output of the task with that id ({{12}} is the twelfth)
 *     {{SELF}}      the id of the task in front (was `$$`)
 *
 * NO QUOTE TRACKING HERE, deliberately. |input| is the value edc_cmd_unquote produced --
 * the shell's delimiters are already gone -- so a quote character in it is content the
 * user typed, never the start of a quoted region. Both this function and
 * edc_shell_collect_var_refs used to re-derive that state from the text; any apostrophe
 * re-opened "single quotes" and silenced every reference after it, so an input like
 * "it's {{LAST}}" expanded nothing. Whether a reference is LIVE is decided one layer up,
 * by the lexer's `interp` flag (edc_lex.l) -> has_var_refs -> has_deferred_vars, and the
 * callers only come here when that is set (the raw forms -- 'x', '''x''' and $'x' -- never
 * set it), so a literal region cannot reach this code. The corollary a user can notice: a
 * quote INSIDE a double-quoted string is content, so "a '{{1}}' b" expands; only
 * 'a {{1}} b' is literal.
 *
 * Pure string handling, depending on no app/session/scheduler --
 * placed in //src/shell:shell_parse so //src/app:app can depend on it directly,
 * avoiding an app <-> shell static-link cycle. */

static bool all_digits(const char *s, size_t n)
{
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

sds edc_shell_expand_vars(const char *input, edc_shell_var_resolver_fn resolver, void *user_data, sds *out_error)
{
    /* Out-param contract: ALWAYS written before returning -- NULL on success, a message
     * on failure. A caller can pass an uninitialized `sds err;` without zeroing it first
     * (edc_shell_spawn_task uses the same contract). */
    if (out_error)
        *out_error = NULL;
    sds result = sdsempty();
    const char *p = input;

    while (*p)
    {
        if (p[0] != '{' || p[1] != '{')
        {
            result = sdscatlen(result, p, 1);
            p++;
            continue;
        }

        const char *close = strstr(p + 2, "}}");
        if (!close)
        {
            /* unterminated: the two bytes are literal text */
            result = sdscatlen(result, "{{", 2);
            p += 2;
            continue;
        }
        const char *after = close + 2;
        sds body = sdsnewlen(p + 2, (size_t)(close - (p + 2)));

        const char *key = NULL;
        if (strcmp(body, "LAST") == 0)
            key = "LAST";
        else if (strcmp(body, "SELF") == 0)
            key = "SELF";
        else if (all_digits(body, sdslen(body)))
            key = body;

        if (!key)
        {
            /* Not one of our forms: copy it VERBATIM (no error -- see the header). */
            result = sdscatprintf(result, "{{%s}}", body);
            sdsfree(body);
            p = after;
            continue;
        }

        sds val = resolver(key, user_data);
        if (!val)
        {
            if (out_error)
                *out_error = sdscatprintf(sdsempty(), "{{%s}}: no output", key);
            sdsfree(body);
            sdsfree(result);
            return NULL;
        }
        result = sdscatsds(result, val);
        sdsfree(val);
        sdsfree(body);
        p = after;
    }

    return result;
}
