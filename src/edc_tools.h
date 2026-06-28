#include "sds.h"
#ifndef __BC_TOOLS_H__
#define __BC_TOOLS_H__

#include "cJSON.h"
#include "edc_error.h"
#include "edc_param_types.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <stdbool.h>

typedef void (*EdcDestroyNotify)(void *data);

struct edc_tool_param
{
    sds name;
    struct edc_param_type *type;
    bool required;
    struct edc_tool_param *next;
};

extern struct edc_tool_param *edc_tool_param_new(char *name, struct edc_param_type *type, bool required);
extern void edc_tool_param_free(struct edc_tool_param *param);

struct edc_toolbox;
struct edc_tool;
struct edc_toolbox_call_ctx;
typedef void (*edc_tool_exec_async_fn)(struct edc_toolbox_call_ctx *ctx, struct edc_tool *, cJSON *,
                                       void (*callback)(struct edc_toolbox_call_ctx *ctx, sds , EdcError *));

struct edc_tool
{
    sds name;
    sds desc;
    struct edc_tool_param *params;
    void *priv;

    edc_tool_exec_async_fn exec;
    EdcDestroyNotify priv_free_fn;
    UT_hash_handle hh;
};

extern struct edc_tool *edc_tool_new(const char *name, const char *desc, struct edc_tool_param *params,
                                     edc_tool_exec_async_fn exec);
extern void edc_tool_free(struct edc_tool *tool);
extern struct edc_tool *edc_tool_new_v(const char *name, const char *desc, struct edc_tool_param **params,
                                       edc_tool_exec_async_fn exec);
#endif
