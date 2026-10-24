#include "edc_toolbox.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_param_types.h"
#include "edc_tools.h"
#include "edc_tools_crontab.h"
#include "edc_tools_device_file.h"
#include "edc_tools_device_shell.h"
#include "edc_tools_fs.h"
#include "edc_tools_search.h"
#include "edc_tools_shell.h"
#include "edc_tools_todo.h"
#include "edc_tools_memory.h"
#include "edc_task.h" /* the sink, which decides what a task may run (edc_tool_scope_for_task) */
#include "log.h"
#include "sds.h"
#include "uthash.h"
#include <string.h>

static void edc_toolbox_emit_tool_message_internal(struct edc_toolbox_call_ctx *ctx, const char *header,
                                                   const char *content, edc_render_pref_t render_pref)
{
    if (!ctx || !ctx->tb || !content || !*content)
        return;

    struct edc_toolbox *tb = ctx->tb;
    if (!tb->scheduler)
        return;

    /* Routed through the scheduler, not because "on-screen needs its approval" (the
     * display gate is in the frontend) but because ONLY IT knows whether this request is
     * still alive: an async tool callback may arrive after the task was reclaimed, while
     * the caller holds a task pointer -- sending straight to the channel would use a
     * possibly-dangling pointer. It looks up by request_id and also does the owner check
     * and the todowrite dirty-list push; display and message shape still belong to the
     * channel. */
    struct edc_toolbox_emit_msg *emit = g_new0(struct edc_toolbox_emit_msg, 1);
    emit->header = (header && *header) ? sdsnew(header) : NULL;
    emit->content = sdsnew(content);
    emit->render_pref = render_pref;

    edc_actor_send(tb->scheduler,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_TOOL_EMIT, ctx->call_from_type, tb->channel_actor,
                                              ctx->request_id, emit, edc_toolbox_emit_msg_free));
}

void edc_toolbox_emit_tool_message_plain(struct edc_toolbox_call_ctx *ctx, const char *header, const char *content)
{
    /* The tool explicitly declares plain-text output (code/command output/confirmations); shown as-is */
    edc_toolbox_emit_tool_message_internal(ctx, header, content, EDC_RENDER_PLAIN);
}

void edc_toolbox_emit_tool_message_md(struct edc_toolbox_call_ctx *ctx, const char *header, const char *content)
{
    /* The tool explicitly declares Markdown-rendered output */
    edc_toolbox_emit_tool_message_internal(ctx, header, content, EDC_RENDER_MARKDOWN);
}

void edc_toolbox_emit_msg_free(void *p)
{
    if (!p)
        return;
    struct edc_toolbox_emit_msg *emit = p;
    if (emit->header)
        sdsfree(emit->header);
    if (emit->content)
        sdsfree(emit->content);
    free(emit);
}

struct edc_toolbox_call_ctx *edc_toolbox_call_ctx_new(struct edc_toolbox *tb, edc_channel_type channel_type,
                                                      void *request_id, struct edc_task *task, void *priv)
{
    struct edc_toolbox_call_ctx *ctx = g_new0(struct edc_toolbox_call_ctx, 1);
    ctx->tb = tb;
    ctx->call_from_type = channel_type;
    ctx->request_id = request_id;
    ctx->task = task;
    ctx->priv = priv;
    return ctx;
}

void edc_toolbox_call_ctx_free(struct edc_toolbox_call_ctx *ctx)
{
    if (!ctx)
        return;
    free(ctx);
}

struct edc_toolbox *edc_toolbox_new(struct edc_config *conf, edc_actor_t *scheduler, edc_actor_t *channel_actor)
{
    struct edc_toolbox *tb = g_new0(struct edc_toolbox, 1);
    tb->conf = conf;
    tb->schemas = cJSON_CreateArray();
    tb->memory_schemas = cJSON_CreateArray();
    tb->scheduler = scheduler;
    tb->channel_actor = channel_actor;

    edc_toolbox_register_defaults(tb);
    return tb;
}

void edc_toolbox_free(struct edc_toolbox *tb)
{
    if (!tb)
        return;
    edc_tool_shell_abort_all();
    {
        struct edc_tool *t, *tmp;
        HASH_ITER(hh, tb->tools, t, tmp)
        {
            HASH_DEL(tb->tools, t);
            edc_tool_free(t);
        }
    }
    if (tb->schemas)
        cJSON_Delete(tb->schemas);
    if (tb->memory_schemas)
        cJSON_Delete(tb->memory_schemas);
    free(tb);
}

static cJSON *edc_tool_params_to_json(struct edc_tool_param *params)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "object");

    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();

    struct edc_tool_param *param;
    LL_FOREACH(params, param)
    {
        cJSON *item = edc_param_type_to_json(param->type);

        cJSON_AddItemToObject(properties, param->name, item);
        if (param->required)
        {
            cJSON_AddItemToArray(required, cJSON_CreateString(param->name));
        }
    }

    cJSON_AddItemToObject(result, "properties", properties);
    cJSON_AddItemToObject(result, "required", required);

    return result;
}

cJSON *edc_tool_to_schema(const struct edc_tool *tool)
{
    cJSON *function = cJSON_CreateObject();
    cJSON_AddStringToObject(function, "name", tool->name);
    cJSON_AddStringToObject(function, "description", tool->desc);
    cJSON_AddItemToObject(function, "parameters", edc_tool_params_to_json(tool->params));

    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "function");
    cJSON_AddItemToObject(schema, "function", function);

    return schema;
}

void edc_toolbox_register(struct edc_toolbox *tb, struct edc_tool *tool)
{
    HASH_ADD_STR(tb->tools, name, tool);
    cJSON_AddItemToArray(tb->schemas, edc_tool_to_schema(tool));
    /* The memory_* tools are also collected separately: the curation task gets this
     * subset (and only this) instead of the full set. */
    if (tb->memory_schemas && strncmp(tool->name, "memory_", 7) == 0)
        cJSON_AddItemToArray(tb->memory_schemas, edc_tool_to_schema(tool));
}

void edc_toolbox_register_defaults(struct edc_toolbox *tb)
{
    edc_tools_fs_register(tb);
    edc_tools_shell_register(tb);
    edc_tools_search_register(tb);
    edc_tools_crontab_register(tb);
    edc_tools_todo_register(tb);
    edc_tools_memory_register(tb);
    edc_tools_device_shell_register(tb);
    edc_tools_device_file_register(tb);
}

static bool edc_tool_validate_params(const struct edc_tool *tool, cJSON *args, EdcError **error)
{
    if (!args || !cJSON_IsObject(args))
    {
        edc_error_set(error, EDC_ERR_INVAL, "arguments must be a JSON object");
        return false;
    }

    struct edc_tool_param *param;
    LL_FOREACH(tool->params, param)
    {
        cJSON *item = cJSON_GetObjectItem(args, param->name);

        if (!item || cJSON_IsNull(item))
        {
            if (param->required)
            {
                edc_error_set(error, EDC_ERR_INVAL, "required parameter '%s' is missing", param->name);
                return false;
            }
            continue;
        }

        struct edc_param_type *type = param->type;
        bool valid = edc_param_type_validate(item, type);

        if (!valid)
        {
            sds type_str = edc_param_type_to_s(type);
            edc_error_set(error, EDC_ERR_INVAL, "parameter '%s' must be of type '%s'", param->name, type_str);
            sdsfree(type_str);
            return false;
        }
    }

    return true;
}

/* Which tool kinds a task may RUN. This is NOT the declaration set: that one decides what
 * the model is told about (edc_toolset), while this decides what actually executes. They
 * differ on purpose -- a deferred tool is callable without being declared, and a curator
 * must not be dragged into shell work just because the model asked (docs/pi-toolset-plan.zh.md,
 * I4 "声明 ≠ 权限"). */
static uint32_t edc_tool_scope_for_task(const struct edc_task *task)
{
    if (!task)
        return EDC_TOOL_SCOPE_NORMAL; /* a non-task path (no per-task policy to apply) */
    switch (task->sink)
    {
    case EDC_SINK_SESSION_MEMORY:
        return EDC_TOOL_SCOPE_MEMORY;
    case EDC_SINK_GROUP_MEMORY:
        /* A fold's one-shot summary declares no tools at all; a stray call must not run. */
        return 0;
    default:
        return EDC_TOOL_SCOPE_NORMAL;
    }
}

/* The names a task of |scope| may actually use, for the rejection message: "not available"
 * without saying what IS available leaves the model guessing. */
static sds edc_toolbox_names_for_scope(const struct edc_toolbox *tb, uint32_t scope)
{
    sds names = sdsempty();
    if (!tb)
        return names;
    struct edc_tool *t, *tmp;
    HASH_ITER(hh, tb->tools, t, tmp)
    {
        if ((t->scope & scope) == 0)
            continue;
        if (sdslen(names) > 0)
            names = sdscat(names, ", ");
        names = sdscatsds(names, t->name);
    }
    return names;
}

/* ── Spilling a capped tool result ───────────────────────────────────────────
 *
 * A result over the budget used to be cut and the remainder FREED: the model got a note
 * saying how much it had lost, and no way to see it. Now the whole result is written once
 * and the message carries a bounded preview plus the path -- the model reads the rest with
 * read_file (which has offset/limit for exactly this).
 *
 * Where: <workspace>/<session>/tool-output/. Two reasons, not one. It is under the workspace,
 * so the sandbox already allows the model to read it back; and it is per session, so a
 * session's spilled output is deleted with the session instead of accumulating forever in a
 * temp directory nobody owns.
 *
 * Writes with edc_file_write_safe(): the target lives in a tree the model can write to, so a
 * planted symlink (tool-output/x.txt -> ~/.edgeclaw/config.json) must not redirect it. */
#define EDC_SPILL_SEQ_MAX 100000

static unsigned g_spill_seq; /* runner-thread only, so a plain counter (no lock) */

sds edc_toolbox_spill_output(struct edc_toolbox *tb, const char *tool_name, const char *session_id,
                             const char *content, size_t len)
{
    if (!tb || !tb->conf || !tb->conf->workspace || !content)
        return NULL;

    sds dir = edc_path_join(tb->conf->workspace, session_id && *session_id,
                            "tool-output", NULL);
    EdcError *error = NULL;
    bool made = edc_dir_create_with_parents(dir, 0755, &error);
    if (error)
        edc_error_free(error);
    if (!made)
    {
        sdsfree(dir);
        return NULL;
    }

    /* A name that cannot collide within a process run, and that says which tool produced it:
     * the model (and a human reading the transcript) can tell them apart at a glance. */
    sds safe_tool = sdsnew(tool_name && *tool_name ? tool_name : "tool");
    for (size_t i = 0; i < sdslen(safe_tool); i++)
    {
        const unsigned char c = (unsigned char)safe_tool[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            safe_tool[i] = '_';
    }

    sds path = NULL;
    for (int attempt = 0; attempt < 100; attempt++)
    {
        unsigned seq = ++g_spill_seq % EDC_SPILL_SEQ_MAX;
        sds candidate = sdscatprintf(sdsempty(), "%s/%s-%u.txt", dir, safe_tool, seq);
        if (!edc_file_exists(candidate))
        {
            path = candidate;
            break;
        }
        sdsfree(candidate);
    }
    sdsfree(safe_tool);
    sdsfree(dir);
    if (!path)
        return NULL; /* no free name: report no path rather than overwrite someone's file */

    error = NULL;
    bool ok = edc_file_write_safe(path, content, (ssize_t)len, tb->conf->workspace, &error);
    if (error)
        edc_error_free(error);
    if (!ok)
    {
        sdsfree(path);
        return NULL;
    }
    return path;
}

void edc_toolbox_exec(struct edc_toolbox_call_ctx *ctx, const char *name, cJSON *args,
                      void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{

    EdcError *error = NULL;

    struct edc_tool *tool;
    HASH_FIND_STR(ctx->tb->tools, name, tool);
    if (tool)
    {
        const uint32_t want = edc_tool_scope_for_task(ctx->task);
        if ((tool->scope & want) == 0)
        {
            sds available = edc_toolbox_names_for_scope(ctx->tb, want);
            edc_error_set(&error, EDC_ERR_INVAL, "Error: %s is not available to this task%s. Available: %s", name,
                          want == 0 ? " (it declares no tools)" : "",
                          sdslen(available) > 0 ? available : "(none)");
            log_info("toolbox: refused %s for task %s (scope %u)", name,
                     ctx->task && edc_id_str(ctx->task->task_id), want);
            sdsfree(available);
            callback(ctx, NULL, error);
            return;
        }

        if (!edc_tool_validate_params(tool, args, &error))
        {
            callback(ctx, NULL, error);
            return;
        }

        /* Record which tool owns this call: the actor needs it to dispatch a cancellation
         * (see edc_tool_abort_fn). Done here because this is the one place a name becomes a
         * tool. */
        ctx->tool = tool;

        tool->exec(ctx, tool, args, callback);
        return;
    }

    sds tool_names = sdsempty();
    {
        struct edc_tool *t, *tmp;
        HASH_ITER(hh, ctx->tb->tools, t, tmp)
        {
            tool_names = sdscatprintf(tool_names, "%s, ", t->name);
        }
    }

    edc_error_set(&error, EDC_ERR_NOTFOUND, "Error: tool %s not found. Available: %s", name, tool_names);
    sdsfree(tool_names);
    callback(ctx, NULL, error);
}
