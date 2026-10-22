#ifndef __EDC_AST_H__
#define __EDC_AST_H__
/* 新输入流水线的 IR。流水线：
 *
 *   用户输入 → edc_lex → edc_classify → [edc_parse] → **passes** → edc_interp → task
 *
 * IR 有三种节点：SHELL 树、COMMAND 节点和 MESSAGE 节点。**SHELL 与 COMMAND 都进文法**：
 * 两者开头各有自己的 token（行首 `+`/引号 vs `TK_SLASH_CMD`），所以文法里没有"任意 token"
 * 的产生式 —— 那正是 naive.y 验出来的 1 shift/reduce + 26 reduce/reduce 冲突来源。MESSAGE
 * 的节点由派发层在分类之后直接构造（它没有语法）。上游怎么造节点是文法的事，下游
 * （passes / 解释器 / 命令层）只认 IR。
 *
 * 每个"消息载荷"（MESSAGE 节点的文本、SHELL 每个 stage 的文本）都用同一个
 * struct edc_msg_payload 表达：pass 就地改它（定稿文本、挂 synthetic 块），解释器只消费
 * 结果。这就是 `@` 文件引用、变量引用等语法能作为"一个 pass"插进流水线的原因。 */
/* struct edc_msg_part：pass 追加的消息（跨层值类型，定义在 common）。 */
#include "edc_msg_part.h"
#include "sds.h"
#include <stdbool.h>

/* 一个消息载荷：用户（或解析）产出的文本 + pass 展开出的附加消息。
 *
 * |text|  ：模型看到的那条用户消息（pass 可以改它，例如去转义）。
 * |parts| ：模型**额外**看到的消息，按 pass 追加的顺序。作为 turn 里独立的消息送出，
 *           绝不拼进 |text| —— 拼进去就会被请求期的 {{…}} 展开改写。
 *
 * 追加用 edc_msg_payload_add_part()；载荷被 edc_ast_free() 释放。 */
struct edc_msg_payload
{
    sds text;
    struct edc_msg_part *parts;
    int nparts;
};

/* 追加一条消息（复制 |content|）。这是 pass 与"模型看到什么"之间唯一的接口。 */
void edc_msg_payload_add_part(struct edc_msg_payload *payload, enum edc_msg_role role, bool synthetic,
                             const char *content);

struct edc_ast_redir
{
    int fd;         /* 1 = stdout, 2 = stderr. fd 0 (`<`) is retired: the grammar rejects
                     * it with a pointer at `@file` (see redir_add in edc_parse.y). */
    bool append;    /* >> / 2>> */
    sds target;
    struct edc_ast_redir *next;
};

struct edc_ast_stage
{
    struct edc_msg_payload msg; /* 已去引号的 word + pass 展开出的块 */
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
    /* The operator that joins this pipeline to the previous one (`;` / `&&` / `||`). The
     * first pipeline has no predecessor, so its value is never read. */
    enum edc_ast_chain
    {
        EDC_CHAIN_SEMI = 0, /* ;  -- order only: the next one runs whatever happened */
        EDC_CHAIN_AND,      /* && -- the next one runs only if this one succeeded */
        EDC_CHAIN_OR,       /* || -- the next one runs only if this one did not */
    } chain;
    struct edc_ast_pipeline *next;
};

/* A `/command` line, produced by the GRAMMAR (not by the classifier): the word after
 * the slash plus its arguments, both already unquoted by the lexer. The command layer
 * dispatches on THIS -- it no longer splits the raw text itself, so a command line and
 * a shell line share one syntax implementation (the lexer plus this grammar). */
struct edc_ast_cmd
{
    sds name;   /* "session" for `/session …` -- the slash is the lexer's delimiter */
    sds *argv;  /* the words after it, already unquoted; NULL when argc == 0 */
    int argc;
};

enum edc_ast_kind
{
    EDC_AST_SHELL = 0, /* 文法产出：`+a | b; c &` 那种 */
    EDC_AST_MESSAGE,   /* 分类产出：整条输入就是一条消息 */
    EDC_AST_COMMAND,   /* 文法产出：`/cmd args…` 那种 */
};

typedef struct edc_ast
{
    enum edc_ast_kind kind;
    /* EDC_AST_MESSAGE */
    struct edc_msg_payload message;
    /* EDC_AST_SHELL */
    bool new_group;  /* `+` */
    bool background; /* 链尾 `&`：作用于整条链的每个 stage */
    struct edc_ast_pipeline *pipelines;
    /* EDC_AST_COMMAND */
    struct edc_ast_cmd *cmd;
} edc_ast;

/* 释放整棵树（含 sds）；对两种 kind 都安全。 */
void edc_ast_free(edc_ast *a);

#endif
