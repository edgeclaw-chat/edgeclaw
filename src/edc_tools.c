#include "edc_tools.h"
#include "edc_toolbox.h"

G_DEFINE_QUARK(edc_tool_error_quark, edc_tool_error)

struct edc_tool_param *edc_tool_param_new(gchar *name, struct edc_param_type *type, gboolean required)
{
    struct edc_tool_param *param = g_new(struct edc_tool_param, 1);
    param->name = g_strdup(name);
    param->type = type;
    param->required = required;
    return param;
}

void edc_tool_param_free(struct edc_tool_param *param)
{
    if (!param)
        return;
    g_free(param->name);
    edc_param_type_free(param->type);
    g_free(param);
}

void edc_tool_free(struct edc_tool *tool)
{
    if (!tool)
        return;
    g_string_free(tool->name, TRUE);
    g_string_free(tool->desc, TRUE);

    g_slist_free_full(tool->params, (GDestroyNotify)edc_tool_param_free);

    if (tool->priv_free_fn && tool->priv)
    {
        tool->priv_free_fn(tool->priv);
    }

    g_free(tool);
}

struct edc_tool *edc_tool_new(const gchar *name, const gchar *desc, GSList *params, edc_tool_exec_async_fn exec)
{
    struct edc_tool *tool = g_new0(struct edc_tool, 1);
    tool->name = g_string_new(name);
    tool->desc = g_string_new(desc);
    tool->params = params;
    tool->exec = exec;
    tool->priv = NULL;
    tool->priv_free_fn = NULL;

    return tool;
}
