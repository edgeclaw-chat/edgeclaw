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
                                       void (*callback)(struct edc_toolbox_call_ctx *ctx, sds, EdcError *));

/* Optional: stop the in-flight call this context belongs to. The toolbox actor calls it
 * (with that call's own context) when the issuing task is cancelled.
 *
 * NULL means "nothing this hook can do", which today is true of most tools for one of two
 * different reasons -- the only one that finishes inside `exec` needs no hook, while the
 * ones that DO have an in-flight window and no implementation yet (the device_* tools) are
 * simply not wired. Do not read NULL as a guarantee that no work is outstanding.
 *
 * A tool that implements it owns the meaning. "Interrupt" is not one thing:
 *   - a local process: kill it (see edc_tools_shell.c);
 *   - work that cannot be preempted mid-step (a file write already handed to the thread
 *     pool): give up now and refuse whatever comes next, but the step in progress cannot
 *     be unsent;
 *   - work that can, once the handle is reachable: signal the worker / close the connection.
 * It must NOT free |ctx|: the actor owns it and still needs it to receive the call's
 * outcome (which will simply be discarded). */
typedef void (*edc_tool_abort_fn)(struct edc_toolbox_call_ctx *ctx);

struct edc_tool
{
    sds name;
    sds desc;
    struct edc_tool_param *params;
    void *priv;

    edc_tool_exec_async_fn exec;
    edc_tool_abort_fn abort; /* optional: NULL = this tool has no interruptible work */
    EdcDestroyNotify priv_free_fn;
    UT_hash_handle hh;
};

extern struct edc_tool *edc_tool_new(const char *name, const char *desc, struct edc_tool_param *params,
                                     edc_tool_exec_async_fn exec);
extern void edc_tool_free(struct edc_tool *tool);
/* Install the tool's abort hook (kept out of edc_tool_new so the 17 existing constructions
 * did not have to grow a NULL argument). */
extern void edc_tool_set_abort(struct edc_tool *tool, edc_tool_abort_fn abort);
extern struct edc_tool *edc_tool_new_v(const char *name, const char *desc, struct edc_tool_param **params,
                                       edc_tool_exec_async_fn exec);
#endif
