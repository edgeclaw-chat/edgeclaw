#ifndef __BC_CMD_PARSE_H__
#define __BC_CMD_PARSE_H__

#include "sds.h"
#include <stdbool.h>

/* task 描述 (desc) 的最大 UTF-8 字符数，超出截断并追加 "..." */
#define EDC_TASK_DESC_MAX 30

/* ── /task 命令解析结果 ────────────────────────────────────── */

struct edc_cmd_task_opts
{
    sds desc;     /* 任务描述 (剩余文本) */
    sds input;    /* 引号内输入 — @ 语法专用 */
    bool bg;      /* --bg 后台 */
    bool literal; /* input 来自 '''/''' 块或单引号：不解析 $ 变量 */
    sds *dep_ids; /* -d <id> 依赖列表 */
    int dep_count;
    sds group_id;    /* --group <id> / -g <id> */
    sds prompt_name; /* --prompt <name> */
};

/* ── API ───────────────────────────────────────────────────── */

/** 解析 task 操作参数。arg 可为 NULL。调用方用 edc_cmd_task_opts_free 释放。 */
extern void edc_cmd_parse_task_opts(sds arg, struct edc_cmd_task_opts *opts);

/** 释放 parse 分配的资源 */
extern void edc_cmd_task_opts_free(struct edc_cmd_task_opts *opts);

#endif
