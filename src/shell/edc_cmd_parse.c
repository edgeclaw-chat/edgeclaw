#include "edc_cmd_parse.h"
#include "edc_string_utils.h"
#include <stdlib.h>
#include <string.h>

#define DESC_MAX EDC_TASK_DESC_MAX

void edc_cmd_parse_task_opts(sds arg, struct edc_cmd_task_opts *opts)
{
    if (!opts)
        return;
    memset(opts, 0, sizeof(*opts));
    if (!arg)
        return;

    sds remaining = sdsnew(arg);

    /* detect trailing & (background) */
    if (sdslen(remaining) > 0 && remaining[sdslen(remaining) - 1] == '&')
    {
        opts->bg = true;
        sdssetlen(remaining, sdslen(remaining) - 1);
        sdstrim(remaining, " \t");
    }

    /*
     * Input extraction follows a Python-style quote grammar:
     *   - argument starts with """ or ''': that whole block, which may span
     *     multiple lines, is taken verbatim as the input (up to the closing
     *     triple quote); anything after it is parsed for flags.
     *   - argument starts with a single '"' or '\'': that quoted chunk is the
     *     input, and everything after its closing quote is parsed for flags.
     *   - otherwise the entire remainder is the input verbatim, so pasted
     *     text that merely contains quotes (e.g. JSON) is never truncated;
     *     flags are not scanned in this mode.
     */
    char *s = remaining;
    while (*s == ' ' || *s == '\t')
        s++;

    bool input_consumed = false;

    if (strncmp(s, "\"\"\"", 3) == 0 || strncmp(s, "'''", 3) == 0)
    {
        const char *delim = (*s == '"') ? "\"\"\"" : "'''";
        /* Python semantics: ''' is raw — no $variable expansion inside.
         * Capture BEFORE sdsrange() below rewrites the buffer s points into
         * (it null-terminates at the new end, clobbering the opening quote). */
        opts->literal = (*s == '\'');
        char *q_end = strstr(s + 3, delim);
        if (q_end)
        {
            opts->input = sdsnewlen(s + 3, (size_t)(q_end - (s + 3)));
            sdsrange(remaining, (int)(q_end + 3 - remaining), -1);
            sdstrim(remaining, " \t");
            input_consumed = true;
        }
    }

    if (!input_consumed && (*s == '"' || *s == '\''))
    {
        char quote = *s;
        /* single-quoted input is raw too (no $ expansion). Same as above:
         * capture before sdsrange() clobbers the buffer s points into. */
        opts->literal = (quote == '\'');
        char *q_end = strchr(s + 1, quote);
        if (q_end)
        {
            opts->input = sdsnewlen(s + 1, (size_t)(q_end - s - 1));
            sdsrange(remaining, (int)(q_end - remaining + 1), -1);
            sdstrim(remaining, " \t");
            input_consumed = true;
        }
    }

    if (!input_consumed)
    {
        /* unbalanced quotes / no leading quote: the whole text is the input */
        opts->input = sdsdup(remaining);
        sdsfree(remaining);
        remaining = NULL;
    }

    /* build desc: first DESC_MAX UTF-8 chars of input, truncated with "..." */
    if (opts->input)
    {
        opts->desc = edc_str_utf8_truncate(opts->input, DESC_MAX, true);
    }

    /* Parse flags from remaining. The block runs even when `remaining` is an
     * empty sds (which happens once a quoted input has consumed the whole
     * argument); the token loop is a no-op then, but `remaining` still has to
     * be released — the old `sdslen(...) > 0` guard skipped the free and leaked
     * one sds per such parse. */
    if (remaining)
    {
        sds work = sdsdup(remaining);
        char *p = work;
        while (*p)
        {
            while (*p == ' ')
                p++;
            if (*p == '\0')
                break;

            if (strncmp(p, "--bg", 4) == 0 && (p[4] == ' ' || p[4] == '\0'))
            {
                opts->bg = true;
                p += 4;
                continue;
            }
            /* --suspended: 建任务但**不立即执行**。放 --bg 之后, 两者互不前缀。 */
            if (strncmp(p, "--suspended", 11) == 0 && (p[11] == ' ' || p[11] == '\0'))
            {
                opts->suspended = true;
                p += 11;
                continue;
            }
            if (strncmp(p, "-d ", 3) == 0)
            {
                p += 3;
                while (*p == ' ')
                    p++;
                char *start = p;
                while (*p && *p != ' ')
                    p++;
                if (p > start)
                {
                    opts->dep_count++;
                    opts->dep_ids = realloc(opts->dep_ids, (size_t)opts->dep_count * sizeof(sds));
                    opts->dep_ids[opts->dep_count - 1] = sdsnewlen(start, p - start);
                }
                continue;
            }
            if (strncmp(p, "--group ", 8) == 0)
            {
                p += 8;
                while (*p == ' ')
                    p++;
                char *s = p;
                while (*p && *p != ' ')
                    p++;
                if (p > s)
                    opts->group_id = sdsnewlen(s, p - s);
                continue;
            }
            if (strncmp(p, "-g ", 3) == 0)
            {
                p += 3;
                while (*p == ' ')
                    p++;
                char *s = p;
                while (*p && *p != ' ')
                    p++;
                if (p > s)
                {
                    if (opts->group_id)
                        sdsfree(opts->group_id);
                    opts->group_id = sdsnewlen(s, p - s);
                }
                continue;
            }
            if (strncmp(p, "--prompt ", 9) == 0)
            {
                p += 9;
                while (*p == ' ')
                    p++;
                char *s = p;
                while (*p && *p != ' ')
                    p++;
                if (p > s)
                    opts->prompt_name = sdsnewlen(s, p - s);
                continue;
            }
            /* unrecognized token — skip until space */
            while (*p && *p != ' ')
                p++;
        }
        sdsfree(work);
        sdsfree(remaining);
        remaining = NULL;
    }
}

void edc_cmd_task_opts_free(struct edc_cmd_task_opts *opts)
{
    if (!opts)
        return;
    if (opts->desc)
        sdsfree(opts->desc);
    if (opts->input)
        sdsfree(opts->input);
    if (opts->group_id)
        sdsfree(opts->group_id);
    if (opts->prompt_name)
        sdsfree(opts->prompt_name);
    for (int i = 0; i < opts->dep_count; i++)
        sdsfree(opts->dep_ids[i]);
    free(opts->dep_ids);
    memset(opts, 0, sizeof(*opts));
}
