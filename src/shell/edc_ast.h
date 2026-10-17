#ifndef __EDC_AST_H__
#define __EDC_AST_H__
/* 新输入流水线的 AST（docs/input-pipeline.zh.md §6）。
 *
 * 只表达 SHELL 那种输入：分类（COMMAND / LOCALSHELL / MESSAGE）由派发层看第一个 token
 * 直接决定，不进文法 —— 否则文法里就要有"任意 token"的产生式，那正是 naive.y 验出来的
 * 1 shift/reduce + 26 reduce/reduce 冲突来源。 */
#include "sds.h"
#include <stdbool.h>

struct edc_ast_redir
{
    int fd;         /* 0 = <, 1 = stdout, 2 = stderr */
    bool append;    /* >> / 2>> */
    sds target;
    struct edc_ast_redir *next;
};

struct edc_ast_stage
{
    sds text;       /* 已去引号的 word */
    bool has_var_refs;
    sds *deps;      /* -d / --with */
    int ndep;
    sds group;      /* -g / --group */
    struct edc_ast_redir *redirs;
    /* 同一 pipeline 内的下一个 stage：边是 JOIN（注入源输出）。 */
    struct edc_ast_stage *next;
};

struct edc_ast_pipeline
{
    struct edc_ast_stage *stages;
    /* 下一个 pipeline（`;` 分隔）：边是 SEQUENCE（只等、不注入）。 */
    struct edc_ast_pipeline *next;
};

typedef struct edc_ast
{
    bool new_group;  /* `+` */
    bool background; /* 链尾 `&`：作用于整条链的每个 stage */
    struct edc_ast_pipeline *pipelines;
} edc_ast;

/* 释放整棵树（含 sds）。 */
void edc_ast_free(edc_ast *a);

#endif
