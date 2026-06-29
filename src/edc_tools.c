#include "edc_string.h"
#include "edc_strutil.h"
#include "edc_tools.h"
#include "edc_toolbox.h"

G_DEFINE_QUARK(edc_tool_error_quark, edc_tool_error)

struct edc_tool_param *edc_tool_param_new(char *name, struct edc_param_type *type, bool required)
{
    struct edc_tool_param *param = g_new(struct edc_tool_param, 1);
    param->name = strdup(name);
    param->type = type;
    param->required = required;
    return param;
}

void edc_tool_param_free(struct edc_tool_param *param)
{
    if (!param)
        return;
    free(param->name);
    edc_param_type_free(param->type);
    free(param);
}

void edc_tool_free(struct edc_tool *tool)
{
    if (!tool)
        return;
    edc_str_free(tool->name);
    edc_str_free(tool->desc);

    g_slist_free_full(tool->params, (EdcDestroyNotify)edc_tool_param_free);

    if (tool->priv_free_fn && tool->priv)
    {
        tool->priv_free_fn(tool->priv);
    }

    free(tool);
}

struct edc_tool *edc_tool_new(const char *name, const char *desc, GSList *params, edc_tool_exec_async_fn exec)
{
    struct edc_tool *tool = g_new0(struct edc_tool, 1);
    tool->name = edc_str_new(name);
    tool->desc = edc_str_new(desc);
    tool->params = params;
    tool->exec = exec;
    tool->priv = NULL;
    tool->priv_free_fn = NULL;

    return tool;
}

struct edc_tool *edc_tool_new_v(const char *name, const char *desc, struct edc_tool_param **params,
                                edc_tool_exec_async_fn exec)
{
    struct edc_tool *tool = g_new0(struct edc_tool, 1);
    tool->name = edc_str_new(name);
    tool->desc = edc_str_new(desc);

    GSList *l = NULL;
    for (int i = 0; params[i]; ++i)
    {
        l = g_slist_append(l, params[i]);
    }
    tool->params = l;
    tool->exec = exec;
    tool->priv = NULL;
    tool->priv_free_fn = NULL;

    return tool;
}
