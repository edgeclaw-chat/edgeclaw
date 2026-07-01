#include "sds.h"
#include "edc_tools.h"
#include "edc_toolbox.h"
#include "edc_compat.h"

struct edc_tool_param *edc_tool_param_new(char *name, struct edc_param_type *type, bool required)
{
    struct edc_tool_param *param = calloc(1, sizeof(*param));
    param->name = sdsnew(name);
    param->type = type;
    param->required = required;
    return param;
}

void edc_tool_param_free(struct edc_tool_param *param)
{
    if (!param)
        return;
    sdsfree(param->name);
    edc_param_type_free(param->type);
    free(param);
}

void edc_tool_free(struct edc_tool *tool)
{
    if (!tool)
        return;

    {
        struct edc_tool_param *p, *tmp;
        LL_FOREACH_SAFE(tool->params, p, tmp)
        {
            LL_DELETE(tool->params, p);
            edc_tool_param_free(p);
        }
    }

    sdsfree(tool->name);
    sdsfree(tool->desc);

    if (tool->priv_free_fn && tool->priv)
    {
        tool->priv_free_fn(tool->priv);
    }

    free(tool);
}

struct edc_tool *edc_tool_new(const char *name, const char *desc, struct edc_tool_param *params,
                                edc_tool_exec_async_fn exec)
{
    struct edc_tool *tool = g_new0(struct edc_tool, 1);
    tool->name = sdsnew(name);
    tool->desc = sdsnew(desc);
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
    tool->name = sdsnew(name);
    tool->desc = sdsnew(desc);

    struct edc_tool_param *l = NULL;
    for (int i = 0; params[i]; ++i)
    {
        LL_APPEND(l, params[i]);
    }
    tool->params = l;
    tool->exec = exec;
    tool->priv = NULL;
    tool->priv_free_fn = NULL;

    return tool;
}
