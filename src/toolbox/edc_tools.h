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

/* Which task kinds may DECLARE a tool (see struct edc_tool.scope). A fold task declares no
 * tools at all, so it has no bit here. */
#define EDC_TOOL_SCOPE_NORMAL (1u << 0) /* an ordinary reply task */
#define EDC_TOOL_SCOPE_MEMORY (1u << 1) /* the session-memory curator's one-shot call */

/* How the model sees a tool (see struct edc_tool.exposure). */
#define EDC_TOOL_EXPOSURE_DIRECT 0   /* full schema in the request's `tools` field */
#define EDC_TOOL_EXPOSURE_DEFERRED 1 /* named in prose only; called by name */

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
    /* One-line selection summary for the system prompt's tool section ("- name: snippet").
     * A snippet is an affordance for CHOOSING a tool, not a second copy of the signature:
     * the request's `tools` field already carries name/description/parameters, so a snippet
     * must never restate the parameters. NULL = the tool is not listed there. */
    sds snippet;
    /* Behavioural notes that belong to no JSON schema field (output caps, ".gitignore"
     * semantics, "device_* runs remotely"). One per line; NULL = none. */
    sds guidelines;
    /* Which kinds of task may declare this tool (bitmask of EDC_TOOL_SCOPE_*). Defaults to
     * NORMAL so a tool registered without a setter behaves exactly as before. This is the
     * DECLARATION bitmask, not a permission gate: the execution-side check lives in
     * edc_toolbox_exec and answers a different question. */
    uint32_t scope;
    /* EDC_TOOL_EXPOSURE_DIRECT = the request's `tools` field carries this tool's full
     * schema; DEFERRED = only the prompt's prose names it and the model calls it by name
     * (the MCP shape, unused until B4). */
    uint8_t exposure;
    struct edc_tool_param *params;
    void *priv;

    edc_tool_exec_async_fn exec;
    edc_tool_abort_fn abort; /* optional: NULL = this tool has no interruptible work */
    /* Declared by the tool: its result must NOT be cut at tool_max_output_size on the way to
     * the model. For a tool whose output IS knowledge rather than bulk (the session memory
     * tools), a silent cut loses facts; such a tool must be able to bound its own output
     * (the memory tools are bounded by the entry table). */
    bool keep_full_output;
    EdcDestroyNotify priv_free_fn;
    UT_hash_handle hh;
};

extern struct edc_tool *edc_tool_new(const char *name, const char *desc, struct edc_tool_param *params,
                                     edc_tool_exec_async_fn exec);
extern void edc_tool_free(struct edc_tool *tool);
/* Set the one-line selection summary shown in the system prompt's tool section (NULL
 * clears it). Do not restate the parameters: the `tools` field already carries them. */
extern void edc_tool_set_snippet(struct edc_tool *tool, const char *snippet);
/* Add newline-separated behavioural notes that no JSON schema field can carry. */
extern void edc_tool_set_guidelines(struct edc_tool *tool, const char *lines);
/* Which task kinds may declare this tool (bitmask of EDC_TOOL_SCOPE_*). */
extern void edc_tool_set_scope(struct edc_tool *tool, uint32_t scope);
/* How the model sees this tool (EDC_TOOL_EXPOSURE_*). DIRECT is the default. */
extern void edc_tool_set_exposure(struct edc_tool *tool, uint8_t exposure);
/* Install the tool's abort hook (kept out of edc_tool_new so the 17 existing constructions
 * did not have to grow a NULL argument). */
extern void edc_tool_set_abort(struct edc_tool *tool, edc_tool_abort_fn abort);
/* Declare that this tool's output must reach the model whole (see keep_full_output). */
extern void edc_tool_set_keep_full_output(struct edc_tool *tool, bool keep);
extern struct edc_tool *edc_tool_new_v(const char *name, const char *desc, struct edc_tool_param **params,
                                       edc_tool_exec_async_fn exec);
#endif
