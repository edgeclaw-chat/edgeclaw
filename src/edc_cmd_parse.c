#include "edc_cmd_parse.h"
#include "edc_string_utils.h"
#include <stdlib.h>
#include <string.h>

#define DESC_MAX 30

void edc_cmd_parse_task_opts(sds arg, struct edc_cmd_task_opts *opts)
{
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    if (!arg) return;

    sds remaining = sdsnew(arg);

    /* detect trailing & (background) */
    if (sdslen(remaining) > 0 && remaining[sdslen(remaining)-1] == '&')
    {
        opts->bg = true;
        sdssetlen(remaining, sdslen(remaining) - 1);
        sdstrim(remaining, " \t");
    }

    /* extract "input": quoted string, or entire remaining text */
    char *q_start = strchr(remaining, '"');
    if (q_start)
    {
        char *q_end = strchr(q_start + 1, '"');
        if (q_end)
        {
            opts->input = sdsnewlen(q_start + 1, q_end - q_start - 1);
            sds before = sdsnewlen(remaining, q_start - remaining);
            sdsrange(remaining, (int)(q_end - remaining + 1), -1);
            sdstrim(remaining, " \t");
            sdsfree(before);
        }
    }

    if (!opts->input && sdslen(remaining) > 0)
    {
        /* no quotes: entire remaining text is input */
        opts->input = sdsdup(remaining);
        sdsfree(remaining);
        remaining = NULL;
    }

    /* build desc: first DESC_MAX UTF-8 chars of input, truncated with "..." */
    if (opts->input)
    {
        opts->desc = edc_str_utf8_truncate(opts->input, DESC_MAX, true);
    }

    /* parse flags from remaining (only when quoted input was extracted) */
    if (remaining && sdslen(remaining) > 0)
    {
        sds work = sdsdup(remaining);
        char *p = work;
        while (*p)
        {
            while (*p == ' ') p++;
            if (*p == '\0') break;

            if (strncmp(p, "--bg", 4) == 0 && (p[4] == ' ' || p[4] == '\0'))
                { opts->bg = true; p += 4; continue; }
            if (strncmp(p, "-d ", 3) == 0)
            {
                p += 3; while (*p == ' ') p++;
                char *start = p;
                while (*p && *p != ' ') p++;
                if (p > start)
                {
                    opts->dep_count++;
                    opts->dep_ids = realloc(opts->dep_ids, (size_t)opts->dep_count * sizeof(sds));
                    opts->dep_ids[opts->dep_count - 1] = sdsnewlen(start, p - start);
                }
                continue;
            }
            if (strncmp(p, "--group ", 8) == 0)
                { p += 8; while (*p == ' ') p++;
                  char *s = p; while (*p && *p != ' ') p++;
                  if (p > s) opts->group_id = sdsnewlen(s, p - s); continue; }
            if (strncmp(p, "-g ", 3) == 0)
                { p += 3; while (*p == ' ') p++;
                  char *s = p; while (*p && *p != ' ') p++;
                  if (p > s) { if (opts->group_id) sdsfree(opts->group_id);
                               opts->group_id = sdsnewlen(s, p - s); } continue; }
            if (strncmp(p, "--prompt ", 9) == 0)
                { p += 9; while (*p == ' ') p++;
                  char *s = p; while (*p && *p != ' ') p++;
                  if (p > s) opts->prompt_name = sdsnewlen(s, p - s); continue; }
            /* unrecognized token — skip until space */
            while (*p && *p != ' ') p++;
        }
        sdsfree(work);
        sdsfree(remaining);
    }
}

void edc_cmd_task_opts_free(struct edc_cmd_task_opts *opts)
{
    if (!opts) return;
    if (opts->desc) sdsfree(opts->desc);
    if (opts->input) sdsfree(opts->input);
    if (opts->group_id) sdsfree(opts->group_id);
    if (opts->prompt_name) sdsfree(opts->prompt_name);
    for (int i = 0; i < opts->dep_count; i++) sdsfree(opts->dep_ids[i]);
    free(opts->dep_ids);
    memset(opts, 0, sizeof(*opts));
}
