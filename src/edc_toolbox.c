#include "edc_toolbox.h"
#include "edc_param_types.h"
#include "edc_tools.h"
#include "edc_tools_crontab.h"
#include "edc_tools_fs.h"
#include "edc_tools_shell.h"
#include "edc_tools_todo.h"
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
    g_free(ctx);
}

struct edc_toolbox *edc_toolbox_new(struct edc_config *conf, edc_actor_t *scheduler, edc_actor_t *channel_actor)
{
    struct edc_toolbox *tb = g_new0(struct edc_toolbox, 1);
    tb->tools = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)edc_tool_free);
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
    g_hash_table_destroy(tb->tools);
    if (tb->schemas)
        cJSON_Delete(tb->schemas);
    g_free(tb);
}

static cJSON *edc_tool_params_to_json(GSList *params)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "object");

    cJSON *properties = cJSON_CreateObject();
    cJSON *required = cJSON_CreateArray();

    GSList *p = params;
    while (p)
    {
        struct edc_tool_param *param = p->data;
        p = g_slist_next(p);
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
    cJSON_AddStringToObject(function, "name", tool->name->str);
    cJSON_AddStringToObject(function, "description", tool->desc->str);
    cJSON_AddItemToObject(function, "parameters", edc_tool_params_to_json(tool->params));

    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "function");
    cJSON_AddItemToObject(schema, "function", function);

    return schema;
}

void edc_toolbox_register(struct edc_toolbox *tb, struct edc_tool *tool)
{
    g_hash_table_insert(tb->tools, tool->name->str, tool);
    cJSON_AddItemToArray(tb->schemas, edc_tool_to_schema(tool));

    cJSON *schema = edc_tool_to_schema(tool);
    gchar *s = cJSON_Print(schema);
    log_info("edc_toolbox: register tool: %s", s);
    cJSON_Delete(schema);
    g_free(s);
}

void edc_toolbox_register_defaults(struct edc_toolbox *tb)
{
    edc_tools_fs_register(tb);
    edc_tools_shell_register(tb);
    edc_tools_crontab_register(tb);
    edc_tools_todo_register(tb);
}

static gboolean edc_tool_validate_params(const struct edc_tool *tool, cJSON *args, GError **error)
{
    if (!args || !cJSON_IsObject(args))
    {
        g_set_error(error, BC_TOOL_ERROR, BC_TOOL_ARG_INVALID, "arguments must be a JSON object");
        return FALSE;
    }

    GSList *p = tool->params;
    while (p)
    {
        struct edc_tool_param *param = p->data;
        p = g_slist_next(p);

        cJSON *item = cJSON_GetObjectItem(args, param->name);

        if (!item || cJSON_IsNull(item))
        {
            if (param->required)
            {
                g_set_error(error, BC_TOOL_ERROR, BC_TOOL_ARG_INVALID, "required parameter '%s' is missing",
                            param->name);
                return FALSE;
            }
            continue;
        }

        struct edc_param_type *type = param->type;
        gboolean valid = edc_param_type_validate(item, type);

        if (!valid)
        {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "parameter '%s' must be of type '%s'", param->name,
                        edc_param_type_to_s(type));
            return FALSE;
        }
    }

    return TRUE;
}

void edc_toolbox_exec(struct edc_toolbox_call_ctx *ctx, const gchar *name, cJSON *args,
                     void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{

    GError *error = NULL;

    struct edc_tool *tool = g_hash_table_lookup(ctx->tb->tools, name);
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

    GString *tool_names = g_string_new(NULL);
    GList *head = g_hash_table_get_keys(ctx->tb->tools);
    GList *p = head;
    while (p)
    {
        const gchar *name = p->data;
        g_string_append_printf(tool_names, "%s, ", name);
        p = p->next;
    }

    g_set_error(&error, BC_TOOL_ERROR, BC_TOOL_NOT_FOUND, "Error: tool %s not found. Available: %s", name,
                tool_names->str);
    g_list_free(head);
    g_string_free(tool_names, TRUE);
    callback(ctx, NULL, error);
}
