#include "edc_shell_ast.h"
#include "edc_session_store.h"
#include "edc_cmd_parse.h"
#include "edc_commands.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_shell.h"
#include "edc_string_utils.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

ast_node_t *ast_new(ast_type_t type)
{
    ast_node_t *n = calloc(1, sizeof(ast_node_t));
    n->type = type;
    return n;
}

void ast_free(ast_node_t *node)
{
    if (!node)
        return;
    ast_free(node->child);
    ast_free(node->next);
    ast_free(node->var_refs);
    if (node->text)
        sdsfree(node->text);
    if (node->filename)
        sdsfree(node->filename);
    for (int i = 0; i < node->dep_count; i++)
        sdsfree(node->dep_ids[i]);
    free(node->dep_ids);
    if (node->group_id)
        sdsfree(node->group_id);
    free(node);
}

/* ── AST executor ──────────────────────────────────────────── */

/* The context of one shell execution: the few handles held while walking the AST (the
 * store's reader + dispatch) and the mutable state of the walk. ast_execute builds it
 * from its parameters -- it is not the assembly root: no config / channel lifetime / llm
 * / toolbox here, only what this execution needs. */
typedef struct
{
    struct edc_session_manager *sm;
    struct edc_scheduler *sched;
    struct edc_reply_to to; /* reply address (whom to deliver to / in whose name / which type) */
    struct edc_task_group *target_group;
    struct edc_session *session; /* the session this input belongs to (tasks/groups it creates land here) */
    bool bg;
    edc_task_dep_type dep_type;
    /* The tasks this input creates, gathered so the whole input gets ONE receipt (a
     * `a | b | c` chain used to announce one line per segment). */
    struct edc_task **created;
    size_t created_count;
    size_t created_cap;
} exec_ctx_t;

/* Remember |task| as a product of this input (no-op for NULL). */
static void exec_record_created(exec_ctx_t *ctx, struct edc_task *task)
{
    if (!task)
        return;
    if (ctx->created_count == ctx->created_cap)
    {
        ctx->created_cap = ctx->created_cap ? ctx->created_cap * 2 : 4;
        ctx->created = realloc(ctx->created, ctx->created_cap * sizeof(*ctx->created));
    }
    ctx->created[ctx->created_count++] = task;
}

static struct edc_task *exec_node(
                                  ast_node_t *node, struct edc_task *prev_task, exec_ctx_t *ctx);

static struct edc_task *exec_chain(
                                   ast_node_t *first, exec_ctx_t *ctx)
{
    struct edc_task *prev = NULL;
    ast_node_t *cur = first;
    while (cur)
    {
        prev = exec_node(cur, prev, ctx);
        cur = cur->next;
    }
    return prev;
}

static struct edc_task *exec_node(
                                  ast_node_t *node, struct edc_task *prev_task, exec_ctx_t *ctx)
{
    if (!node)
        return prev_task;

    switch (node->type)
    {
    case AST_ROOT:
        if (node->child)
            return exec_node(node->child, prev_task, ctx);
        return prev_task;

    case AST_SEQ:
        ctx->dep_type = TASK_DEP_SEQUENCE;
        return exec_chain(node->child, ctx);

    case AST_PIPE:
        ctx->dep_type = TASK_DEP_JOIN;
        return exec_chain(node->child, ctx);

    case AST_BG:
        ctx->bg = true;
        return exec_node(node->child, prev_task, ctx);

    case AST_REDIR_OUT:
    case AST_REDIR_APPEND:
    case AST_REDIR_ERR:
    case AST_REDIR_IN: {
        struct edc_task *task = exec_node(node->child, prev_task, ctx);
        if (!task)
            return prev_task;
        if (node->filename)
        {
            switch (node->type)
            {
            case AST_REDIR_OUT:
                if (task->redirect_stdout)
                    sdsfree(task->redirect_stdout);
                task->redirect_stdout = sdsnew(node->filename);
                task->redirect_append = false;
                break;
            case AST_REDIR_APPEND:
                if (task->redirect_stdout)
                    sdsfree(task->redirect_stdout);
                task->redirect_stdout = sdsnew(node->filename);
                task->redirect_append = true;
                break;
            case AST_REDIR_ERR:
                if (task->redirect_stderr)
                    sdsfree(task->redirect_stderr);
                task->redirect_stderr = sdsnew(node->filename);
                break;
            case AST_REDIR_IN: {
                sds err = NULL;
                sds content = edc_shell_read_file(node->filename, &err);
                if (content)
                {
                    if (task->active_turn)
                    {
                        for (size_t mj = task->active_turn->messages.head; mj < task->active_turn->messages.tail; mj++)
                        {
                            cJSON *msg = task->active_turn->messages.data[mj];
                            cJSON *role = cJSON_GetObjectItem(msg, "role");
                            if (role && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0)
                            {
                                cJSON *c = cJSON_GetObjectItem(msg, "content");
                                if (c && cJSON_IsString(c))
                                {
                                    sds merged = sdscatprintf(sdsempty(), "%s\n\n%s", c->valuestring, content);
                                    cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(merged));
                                    sdsfree(merged);
                                }
                                break;
                            }
                        }
                    }
                    sdsfree(content);
                }
                else if (err)
                {
                    edc_commands_reply(ctx->to, err);
                    sdsfree(err);
                }
                break;
            }
            default:
                break;
            }
        }
        return task;
    }

    case AST_CMD: {
        struct edc_cmd_task_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.bg = ctx->bg || node->bg;
        ctx->bg = false; /* reset for next sibling */
        if (node->text && sdslen(node->text) > 0)
        {
            opts.input = sdsnew(node->text);
            /* derive the display description the same way /task does */
            opts.desc = edc_str_utf8_truncate(node->text, EDC_TASK_DESC_MAX, true);
        }
        opts.dep_ids = malloc((size_t)node->dep_count * sizeof(sds));
        for (int i = 0; i < node->dep_count; i++)
            opts.dep_ids[i] = sdsnew(node->dep_ids[i]);
        opts.dep_count = node->dep_count;
        if (node->group_id)
            opts.group_id = sdsnew(node->group_id);

        struct edc_task_group *group = NULL;
        if (!node->at_group && opts.group_id)
        {
            group = edc_session_find_group(edc_session_manager_get_active(ctx->sm), opts.group_id);
            if (!group)
            {
                sds reply = sdscatprintf(sdsempty(), "group %s not found", opts.group_id);
                edc_commands_reply(ctx->to, reply);
                sdsfree(reply);
            }
        }
        if (!group && !node->at_group)
            group = ctx->target_group;

        struct edc_task_group *out_group = NULL;
        sds spawn_err = NULL;
        struct edc_task *task =
            edc_shell_spawn_task(ctx->sm, ctx->sched, ctx->to.type, ctx->session, &opts, prev_task, group,
                                 node->at_group ? "@" : "task", &out_group, NULL, false, NULL, ctx->dep_type,
                                 &spawn_err);
        if (spawn_err)
        {
            edc_commands_reply(ctx->to, spawn_err);
            sdsfree(spawn_err);
        }

        if (task && node->has_deferred_vars)
            task->has_deferred_vars = 1;

        /* The receipt is sent once, at the end of the whole input (ast_execute): the user
         * learns what their line became, not one line per pipe segment. */
        exec_record_created(ctx, task);

        edc_cmd_task_opts_free(&opts);

        if (!ctx->target_group && out_group)
            ctx->target_group = out_group;

        /* No foreground bookkeeping here: `adopt` already decided it when it placed
         * the task (take_foreground = !bg). An explicitly background task must not
         * touch the foreground concept at all -- not even when the session has no
         * foreground group, which is exactly when this used to promote it. */

        return task;
    }

    case AST_DEP: {
        /* dependency declaration: add dep on dep_ids[0] to prev_task.
         * Resolution is global — the dependency may live in another session
         * (task ids are globally unique). An unknown id fails the command. */
        if (prev_task && node->dep_count > 0 && node->dep_ids[0])
        {
            struct edc_task *dt = edc_session_store_resolve_task(ctx->sm, ctx->sched, node->dep_ids[0], NULL);
            if (dt)
                edc_task_add_dependency(prev_task, dt, TASK_DEP_JOIN);
            else
            {
                sds reply = sdscatprintf(sdsempty(), "Task not found: `%s`", node->dep_ids[0]);
                edc_commands_reply(ctx->to, reply);
                sdsfree(reply);
            }
        }
        return prev_task;
    }

    case AST_VAR_DEF:
        /* variable definition: informational, processed by AST_VAR_ASSIGN */
        return prev_task;

    case AST_VAR_ASSIGN: {
        /* collect $VAR references: validate + get deps, then inject into inner CMD */
        sds *var_deps = NULL;
        int var_dep_count = 0;
        sds var_err = NULL;
        sds processed_text = NULL;

        if (node->text && sdslen(node->text) > 0 && strchr(node->text, '$'))
        {
            processed_text =
            edc_shell_collect_var_refs(ctx->sm, ctx->sched, node->text, &var_deps, &var_dep_count, &var_err);
            if (!processed_text)
            {
                edc_commands_reply(ctx->to, var_err);
                sdsfree(var_err);
                return prev_task;
            }
        }

        /* find the AST_CMD leaf and inject processed text + var deps */
        ast_node_t *cmd = node->child;
        while (cmd && cmd->type != AST_CMD)
            cmd = cmd->child;

        if (cmd && processed_text)
        {
            /* replace cmd text with processed (validated) version */
            if (cmd->text)
                sdsfree(cmd->text);
            cmd->text = processed_text;
            processed_text = NULL; /* ownership transferred */

            /* merge var deps into cmd->dep_ids */
            if (var_dep_count > 0)
            {
                int new_count = cmd->dep_count + var_dep_count;
                cmd->dep_ids = realloc(cmd->dep_ids, (size_t)new_count * sizeof(sds));
                for (int i = 0; i < var_dep_count; i++)
                    cmd->dep_ids[cmd->dep_count + i] = var_deps[i];
                cmd->dep_count = new_count;
                free(var_deps);
                var_deps = NULL;
                var_dep_count = 0;
            }
        }

        /* build var_refs chain: AST_DEP nodes for each dependency */
        ast_node_t **vr_tail = &node->var_refs;
        if (cmd)
        {
            for (int i = 0; i < cmd->dep_count; i++)
            {
                if (!cmd->dep_ids[i])
                    continue;
                /* AST_DEP: dependency declaration */
                ast_node_t *dep = ast_new(AST_DEP);
                dep->dep_ids = malloc(sizeof(sds));
                dep->dep_ids[0] = sdsnew(cmd->dep_ids[i]);
                dep->dep_count = 1;
                *vr_tail = dep;
                vr_tail = &dep->next;

                /* AST_VAR_DEF: variable definition (track $VAR → task_id mapping) */
                ast_node_t *vdef = ast_new(AST_VAR_DEF);
                vdef->filename = sdsnew(cmd->dep_ids[i]); /* task_id */
                *vr_tail = vdef;
                vr_tail = &vdef->next;
            }
        }

        /* execute the child CMD (now with injected text + deps) */
        struct edc_task *task = exec_node(node->child, prev_task, ctx);

        /* mark for deferred expansion in edc_app_cb_send_llm */
        if (task && var_dep_count >= 0)
        {
            /* check if any $VAR references exist (even $LAST which has no deps) */
            if (node->text && strstr(node->text, "$"))
                task->has_deferred_vars = 1;
        }

        /* cleanup */
        if (processed_text)
            sdsfree(processed_text);
        for (int i = 0; i < var_dep_count; i++)
            sdsfree(var_deps[i]);
        free(var_deps);

        return task;
    }
    }
    return prev_task;
}

void ast_execute(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to, ast_node_t *root,
                 struct edc_session *session)
{
    if (!root)
        return;
    exec_ctx_t ctx = {0};
    ctx.sm = sm;
    ctx.sched = sched;
    ctx.to = to;
    ctx.dep_type = TASK_DEP_SEQUENCE;
    ctx.session = session;
    /* The foreground group is a session attribute -- read it straight from the passed-in
     * session (no more asking the scheduler for its "currently bound" copy). */
    ctx.target_group = session ? session->foreground_group : NULL;
    if (!ctx.target_group || ctx.target_group->is_orphaned || ctx.target_group->member_count == 0)
        ctx.target_group = NULL;
    exec_node(root, NULL, &ctx);

    /* One receipt for the whole input: what this line became, if there is anything the UI
     * does not already show (background / dependencies). A plain creation is silent. */
    edc_commands_announce_tasks(to, ctx.created, ctx.created_count);
    free(ctx.created);
}
