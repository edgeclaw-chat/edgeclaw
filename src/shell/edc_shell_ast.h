#ifndef __EDC_SHELL_AST_H__
#define __EDC_SHELL_AST_H__

#include "edc_channel_actor.h"
#include "edc_shell_token.h"
#include "edc_task.h"
#include "sds.h"
#include <stdbool.h>

struct edc_session;

struct edc_scheduler;
struct edc_session_manager;
struct edc_actor;

typedef enum
{
    AST_ROOT,         /* top-level: wraps the full input chain */
    AST_SEQ,          /* ; sequential (TASK_DEP_SEQUENCE) */
    AST_PIPE,         /* | pipe chain (TASK_DEP_JOIN) */
    AST_BG,           /* & background modifier */
    AST_REDIR_OUT,    /* > file */
    AST_REDIR_APPEND, /* >> file */
    AST_REDIR_ERR,    /* 2> file */
    AST_REDIR_IN,     /* < file */
    AST_CMD,          /* leaf: text + options */
    AST_DEP,          /* dependency on a task (dep_ids[0] = task_id) */
    AST_VAR_DEF,      /* $VAR reference definition (text = "$1", dep_ids[0] = task_id) */
    AST_VAR_ASSIGN,   /* wraps a CMD with deferred var expansion (child = CMD, var_refs = DEP/VAR_DEF chain) */
} ast_type_t;

typedef struct ast_node
{
    ast_type_t type;
    struct ast_node *child;    /* wrapped node (for BG/REDIR/VAR_ASSIGN) */
    struct ast_node *next;     /* sibling in chain (for SEQ/PIPE/DEP/VAR_DEF) */
    struct ast_node *var_refs; /* for AST_VAR_ASSIGN: linked list of AST_DEP / AST_VAR_DEF (via next) */

    /* AST_CMD fields */
    bool at_group; /* @ prefix */
    bool bg;       /* --bg flag */
    sds text;
    sds *dep_ids;
    int dep_count;
    sds group_id;
    sds prompt_name;
    bool has_deferred_vars;

    /* AST_REDIR_* / AST_VAR_DEF filename (repurposed as task_id for VAR_DEF) */
    sds filename;
} ast_node_t;

ast_node_t *ast_new(ast_type_t type);
void ast_free(ast_node_t *node);

/* Parse tokens into AST. is_at: @ prefix on the input. */
ast_node_t *ast_parse(edc_shell_token_t *tokens, int count, bool is_at);

/* Walk the AST and execute via agent. */
/* |session| = 这条输入所属的 session (调用方由消息的 session 标签解析, 见
 * edc_session_store_resolve_session)。命令建出来的任务/组都落在它上面 —— P3 之后
 * scheduler 不再有"当前 session", 所以必须显式传。 */
void ast_execute(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to, ast_node_t *root,
                 struct edc_session *session);

#endif
