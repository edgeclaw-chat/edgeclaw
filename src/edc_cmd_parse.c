#include "edc_cmd_parse.h"
#include <stdlib.h>
#include <string.h>

/* ── 参数解析 ──────────────────────────────────────────────── */

void edc_cmd_parse_task_opts(sds arg, struct edc_cmd_task_opts *opts)
{
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));

    if (!arg) return;

    sds desc = sdsempty();
    sds remaining = sdsnew(arg);

    /* extract "input": first double-quoted string */
    char *p = remaining;
    char *q_start = strchr(p, '"');
    if (q_start)
    {
        char *q_end = strchr(q_start + 1, '"');
        if (q_end)
        {
            opts->input = sdsnewlen(q_start + 1, q_end - q_start - 1);
            /* remove quoted string from remaining */
            sds before = sdsnewlen(remaining, q_start - remaining);
            sdsrange(remaining, (int)(q_end - remaining + 1), -1);
            sdstrim(remaining, " \t");
            sdsfree(before);
        }
    }

    if (sdslen(remaining) == 0) { sdsfree(remaining); goto finish; }

    /* extract known flags while building desc from leftovers */
    sds work = sdsdup(remaining);
    p = work;
    char *desc_start = p;
    while (*p)
    {
        if (strncmp(p, "--bg", 4) == 0 && (p[4] == ' ' || p[4] == '\0'))
        {
            opts->bg = true;
            /* remove from desc area */
            if (p > desc_start) { sds part = sdsnewlen(desc_start, p - desc_start); desc = sdscatsds(desc, part); sdsfree(part); }
            p += 4; while (*p == ' ') p++;
            if (*p == '\0') break;
            desc_start = p;
            continue;
        }
        if (strncmp(p, "-d ", 3) == 0)
        {
            if (p > desc_start) { sds part = sdsnewlen(desc_start, p - desc_start); desc = sdscatsds(desc, part); sdsfree(part); }
            p += 3; while (*p == ' ') p++;
            char *id_start = p;
            while (*p && *p != ' ') p++;
            if (p > id_start)
            {
                opts->dep_count++;
                opts->dep_ids = realloc(opts->dep_ids, (size_t)opts->dep_count * sizeof(sds));
                opts->dep_ids[opts->dep_count - 1] = sdsnewlen(id_start, p - id_start);
            }
            while (*p == ' ') p++;
            if (*p == '\0') break;
            desc_start = p;
            continue;
        }
        if (strncmp(p, "--group ", 8) == 0)
        {
            if (p > desc_start) { sds part = sdsnewlen(desc_start, p - desc_start); desc = sdscatsds(desc, part); sdsfree(part); }
            p += 8; while (*p == ' ') p++;
            char *g_start = p;
            while (*p && *p != ' ') p++;
            opts->group_id = sdsnewlen(g_start, p - g_start);
            while (*p == ' ') p++;
            if (*p == '\0') break;
            desc_start = p;
            continue;
        }
        if (strncmp(p, "-g ", 3) == 0)
        {
            if (p > desc_start) { sds part = sdsnewlen(desc_start, p - desc_start); desc = sdscatsds(desc, part); sdsfree(part); }
            p += 3; while (*p == ' ') p++;
            char *g_start = p;
            while (*p && *p != ' ') p++;
            if (opts->group_id) sdsfree(opts->group_id);
            opts->group_id = sdsnewlen(g_start, p - g_start);
            while (*p == ' ') p++;
            if (*p == '\0') break;
            desc_start = p;
            continue;
        }
        if (strncmp(p, "--prompt ", 9) == 0)
        {
            if (p > desc_start) { sds part = sdsnewlen(desc_start, p - desc_start); desc = sdscatsds(desc, part); sdsfree(part); }
            p += 9; while (*p == ' ') p++;
            char *pr_start = p;
            while (*p && *p != ' ') p++;
            opts->prompt_name = sdsnewlen(pr_start, p - pr_start);
            while (*p == ' ') p++;
            if (*p == '\0') break;
            desc_start = p;
            continue;
        }
        p++;
    }
    /* append remaining desc */
    if (p > desc_start) { sds part = sdsnewlen(desc_start, p - desc_start); desc = sdscatsds(desc, part); sdsfree(part); }

    sdsfree(work);
    sdsfree(remaining);

finish:
    sdstrim(desc, " \t");
    if (sdslen(desc) > 0) opts->desc = desc; else sdsfree(desc);
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
