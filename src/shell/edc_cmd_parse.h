#ifndef __BC_CMD_PARSE_H__
#define __BC_CMD_PARSE_H__

#include "sds.h"
#include <stdbool.h>

/* Max UTF-8 characters in a task description (desc); longer is truncated with "...". */
#define EDC_TASK_DESC_MAX 30

/* ── /task command parse result ────────────────────────────── */

struct edc_cmd_task_opts
{
    sds desc;     /* task description (the remaining text) */
    sds input;    /* quoted input -- @ syntax only */
    bool bg;        /* --bg background */
    bool suspended; /* --suspended: create suspended, do not run now, wait for /task resume */
    bool literal; /* input came from a '''/''' block or single quotes: do not expand $VARs */
    sds *dep_ids; /* -d <id> dependency list */
    int dep_count;
    sds group_id;    /* --group <id> / -g <id> */
    sds prompt_name; /* --prompt <name> */
};

/* ── API ───────────────────────────────────────────────────── */

/** Parse task operation arguments. arg may be NULL; the caller frees with edc_cmd_task_opts_free. */
extern void edc_cmd_parse_task_opts(sds arg, struct edc_cmd_task_opts *opts);

/** Free the resources parse allocated. */
extern void edc_cmd_task_opts_free(struct edc_cmd_task_opts *opts);

#endif
