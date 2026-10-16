#include "edc_shell_vars.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

/* ── Task references (the expand phase, called once deps are done) ──
 *
 * The sigil is `{{…}}`, NOT `$`: `$` belongs to the PROMPT TEMPLATE layer (`$1`, `$@`,
 * `${1:-x}` -- the pi / Claude Code / opencode convention, so their templates run here
 * unedited). One sigil per layer, and they cannot collide.
 *
 * Recognised forms -- and ONLY these; everything else is copied VERBATIM, because a
 * reference must be spelled exactly to count (that is what makes pasted text safe: a
 * Jinja `{{ var }}`, a stray brace, a `$HOME`, a `$5.00` all pass through untouched):
 *
 *     {{LAST}}      the last completed task's output
 *     {{<digits>}}  the output of the task with that id ({{12}} is the twelfth)
 *     {{SELF}}      the id of the task in front (was `$$`)
 *
 * `'…'` still suppresses expansion (single quotes are the way to write a literal `{{`).
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
    bool in_dq = false, in_sq = false;
    const char *p = input;

    while (*p)
    {
        if (in_dq)
        {
            if (*p == '"')
            {
                in_dq = false;
                result = sdscatlen(result, p, 1);
                p++;
            }
            else if (*p == '\\' && *(p + 1))
            {
                result = sdscatlen(result, p, 1);
                p++;
                result = sdscatlen(result, p, 1);
                p++;
            }
            else if (p[0] == '{' && p[1] == '{')
            {
                p += 2;
                goto expand_ref;
            }
            else
            {
                result = sdscatlen(result, p, 1);
                p++;
            }
        }
        else if (in_sq)
        {
            if (*p == '\'')
                in_sq = false;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '"')
        {
            in_dq = true;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (*p == '\'')
        {
            in_sq = true;
            result = sdscatlen(result, p, 1);
            p++;
        }
        else if (p[0] == '{' && p[1] == '{')
        {
            p += 2;
        expand_ref:
            {
                const char *close = strstr(p, "}}");
                if (!close)
                {
                    /* unterminated: the two bytes are literal text */
                    result = sdscatlen(result, "{{", 2);
                    continue;
                }
                const char *after = close + 2;
                sds body = sdsnewlen(p, (size_t)(close - p));

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
        }
        else
        {
            result = sdscatlen(result, p, 1);
            p++;
        }
    }

    return result;
}
