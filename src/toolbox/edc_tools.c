#include "edc_tools.h"
#include "edc_compat.h"
#include "edc_toolbox.h"
#include "sds.h"
#include "edc_list.h"

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
    sdsfree(tool->snippet);
    sdsfree(tool->guidelines);

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
    /* Defaults keep every existing registration behaving as before: an ordinary tool,
     * declared in the `tools` field. A setter opts into something narrower. */
    tool->scope = EDC_TOOL_SCOPE_NORMAL;
    tool->exposure = EDC_TOOL_EXPOSURE_DIRECT;

    return tool;
}

void edc_tool_set_snippet(struct edc_tool *tool, const char *snippet)
{
    if (!tool)
        return;
    sdsfree(tool->snippet);
    tool->snippet = snippet ? sdsnew(snippet) : NULL;
}

void edc_tool_set_guidelines(struct edc_tool *tool, const char *lines)
{
    if (!tool)
        return;
    sdsfree(tool->guidelines);
    tool->guidelines = lines ? sdsnew(lines) : NULL;
}

void edc_tool_set_scope(struct edc_tool *tool, uint32_t scope)
{
    if (tool)
        tool->scope = scope;
}

void edc_tool_set_exposure(struct edc_tool *tool, uint8_t exposure)
{
    if (tool)
        tool->exposure = exposure;
}

/* Kept out of edc_tool_new so the existing constructions did not have to grow a NULL arg. */
void edc_tool_set_abort(struct edc_tool *tool, edc_tool_abort_fn abort)
{
    if (tool)
        tool->abort = abort;
}

void edc_tool_set_keep_full_output(struct edc_tool *tool, bool keep)
{
    if (tool)
        tool->keep_full_output = keep;
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
        EDC_LL_APPEND(struct edc_tool_param, l, params[i]);
    }
    tool->params = l;
    tool->exec = exec;
    tool->priv = NULL;
    tool->priv_free_fn = NULL;
    tool->scope = EDC_TOOL_SCOPE_NORMAL;
    tool->exposure = EDC_TOOL_EXPOSURE_DIRECT;

    return tool;
}
