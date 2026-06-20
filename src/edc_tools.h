#ifndef __BC_TOOLS_H__
#define __BC_TOOLS_H__

#include "edc_param_types.h"
#include "cJSON.h"
#include <glib.h>

extern GQuark edc_tool_error_quark(void);
#define BC_TOOL_ERROR (edc_tool_error_quark())

struct edc_tool_param
{
    gchar *name;
    struct edc_param_type *type;
    gboolean required;
};

extern struct edc_tool_param *edc_tool_param_new(gchar *name, struct edc_param_type *type, gboolean required);
extern void edc_tool_param_free(struct edc_tool_param *param);

struct edc_toolbox;
struct edc_tool;
struct edc_toolbox_call_ctx;
typedef void (*edc_tool_exec_async_fn)(struct edc_toolbox_call_ctx *ctx, struct edc_tool *, cJSON *,
                                      void (*callback)(struct edc_toolbox_call_ctx *ctx, GString *, GError *));

struct edc_tool
{
    GString *name;
    GString *desc;
    GSList *params;
    void *priv;

    edc_tool_exec_async_fn exec;
    GFreeFunc priv_free_fn;
};

extern struct edc_tool *edc_tool_new(const gchar *name, const gchar *desc, GSList *params, edc_tool_exec_async_fn exec);
extern void edc_tool_free(struct edc_tool *tool);
#endif
