#include "edc_string.h"
#ifndef __BC_TOOLS_H__
#define __BC_TOOLS_H__

#include "cJSON.h"
#include "edc_param_types.h"
#include <glib.h>
#include <stdbool.h>

extern GQuark edc_tool_error_quark(void);
#define BC_TOOL_ERROR (edc_tool_error_quark())

struct edc_tool_param
{
    char *name;
    struct edc_param_type *type;
    bool required;
};

extern struct edc_tool_param *edc_tool_param_new(char *name, struct edc_param_type *type, bool required);
extern void edc_tool_param_free(struct edc_tool_param *param);

struct edc_toolbox;
struct edc_tool;
struct edc_toolbox_call_ctx;
typedef void (*edc_tool_exec_async_fn)(struct edc_toolbox_call_ctx *ctx, struct edc_tool *, cJSON *,
                                       void (*callback)(struct edc_toolbox_call_ctx *ctx, sds , GError *));

struct edc_tool
{
    sds name;
    sds desc;
    GSList *params;
    void *priv;

    edc_tool_exec_async_fn exec;
    GFreeFunc priv_free_fn;
};

extern struct edc_tool *edc_tool_new(const char *name, const char *desc, GSList *params, edc_tool_exec_async_fn exec);
extern void edc_tool_free(struct edc_tool *tool);
extern struct edc_tool *edc_tool_new_v(const char *name, const char *desc, struct edc_tool_param **params,
                                       edc_tool_exec_async_fn exec);
#endif
