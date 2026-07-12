#ifndef __EDC_SHELL_AST_H__
#define __EDC_SHELL_AST_H__

#include "sds.h"
#include "edc_task.h"
#include "edc_shell_token.h"
#include <stdbool.h>

struct edc_agent;
struct edc_actor;
struct edc_channel;

typedef enum {
    AST_ROOT,        /* top-level: wraps the full input chain */
    AST_SEQ,         /* ; sequential (TASK_DEP_SEQUENCE) */
    AST_PIPE,        /* | pipe chain (TASK_DEP_JOIN) */
    AST_BG,          /* & background modifier */
    AST_REDIR_OUT,   /* > file */
    AST_REDIR_APPEND,/* >> file */
    AST_REDIR_ERR,   /* 2> file */
    AST_REDIR_IN,    /* < file */
    AST_CMD,         /* leaf: text + options */
} ast_type_t;

typedef struct ast_node {
    ast_type_t type;
    struct ast_node *child;     /* wrapped node (for BG/REDIR) */
    struct ast_node *next;      /* sibling in chain (for SEQ/PIPE) */

    /* AST_CMD fields */
    bool at_group;              /* @ prefix */
    sds text;
    sds *dep_ids;
    int dep_count;
    sds group_id;
    sds prompt_name;
    bool has_deferred_vars;

    /* AST_REDIR_* filename */
    sds filename;
} ast_node_t;

ast_node_t *ast_new(ast_type_t type);
void ast_free(ast_node_t *node);

/* Parse tokens into AST. is_at: @ prefix on the input. */
ast_node_t *ast_parse(edc_shell_token_t *tokens, int count, bool is_at);

/* Walk the AST and execute via agent. */
void ast_execute(struct edc_agent *agent, struct edc_actor *actor,
                 struct edc_channel *channel, ast_node_t *root);

#endif
