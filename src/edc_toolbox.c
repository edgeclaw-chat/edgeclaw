#include "sds.h"
#include "edc_toolbox.h"
#include "edc_error.h"
#include "edc_compat.h"
#include "edc_param_types.h"
#include "edc_tools.h"
#include "uthash.h"
#include "edc_tools_crontab.h"
#include "edc_tools_fs.h"
#include "edc_tools_shell.h"
#include "edc_tools_todo.h"
#include "edc_tools_device_shell.h"
#include "edc_tools_device_file.h"
#include "log.h"

struct edc_toolbox_call_ctx *edc_toolbox_call_ctx_new(struct edc_toolbox *tb, struct edc_channel *channel, void *priv)
{
    struct edc_toolbox_call_ctx *ctx = g_new0(struct edc_toolbox_call_ctx, 1);
    ctx->tb = tb;
    ctx->call_from = channel;
    ctx->priv = priv;
    return ctx;
}

void edc_toolbox_call_ctx_free(struct edc_toolbox_call_ctx *ctx)
{
    free(ctx);
}

struct edc_toolbox *edc_toolbox_new(struct edc_config *conf, edc_actor_t *scheduler, edc_actor_t *channel_actor)
{
    struct edc_toolbox *tb = g_new0(struct edc_toolbox, 1);
    tb->conf = conf;
    tb->schemas = cJSON_CreateArray();
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
        HASH_ITER(hh, tb->tools, t, tmp) {
            HASH_DEL(tb->tools, t);
            edc_tool_free(t);
        }
    }
    if (tb->schemas)
        cJSON_Delete(tb->schemas);
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

static cJSON *edc_tool_to_schema(const struct edc_tool *tool)
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
}

void edc_toolbox_register_defaults(struct edc_toolbox *tb)
{
    edc_tools_fs_register(tb);
    edc_tools_shell_register(tb);
    edc_tools_crontab_register(tb);
    edc_tools_todo_register(tb);
    edc_tools_device_shell_register(tb);
    edc_tools_device_file_register(tb);
}

static bool edc_tool_validate_params(const struct edc_tool *tool, cJSON *args, EdcError **error)
{
    if (!args || !cJSON_IsObject(args))
    {
        edc_error_set(error, EDC_ERR_INVAL, "arguments must be a JSON object");
        return FALSE;
    }

    struct edc_tool_param *param;
    LL_FOREACH(tool->params, param)
    {
        cJSON *item = cJSON_GetObjectItem(args, param->name);

        if (!item || cJSON_IsNull(item))
        {
            if (param->required)
            {
                edc_error_set(error, EDC_ERR_INVAL, "required parameter '%s' is missing",
                            param->name);
                return FALSE;
            }
            continue;
        }

        struct edc_param_type *type = param->type;
        bool valid = edc_param_type_validate(item, type);

        if (!valid)
        {
            sds type_str = edc_param_type_to_s(type);
            edc_error_set(error, EDC_ERR_INVAL, "parameter '%s' must be of type '%s'", param->name,
                        type_str);
            sdsfree(type_str);
            return FALSE;
        }
    }

    return TRUE;
}

void edc_toolbox_exec(struct edc_toolbox_call_ctx *ctx, const char *name, cJSON *args,
                     void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{

    EdcError *error = NULL;

    struct edc_tool *tool;
    HASH_FIND_STR(ctx->tb->tools, name, tool);
    if (tool)
    {
        if (!edc_tool_validate_params(tool, args, &error))
        {
            callback(ctx, NULL, error);
            return;
        }

        tool->exec(ctx, tool, args, callback);
        return;
    }

    sds tool_names = sdsempty();
    {
        struct edc_tool *t, *tmp;
        HASH_ITER(hh, ctx->tb->tools, t, tmp) {
            tool_names = sdscatprintf(tool_names, "%s, ", t->name);
        }
    }

    edc_error_set(&error, EDC_ERR_NOTFOUND, "Error: tool %s not found. Available: %s", name,
                tool_names);
    sdsfree(tool_names);
    callback(ctx, NULL, error);
}
