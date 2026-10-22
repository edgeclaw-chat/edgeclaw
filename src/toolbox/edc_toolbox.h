#include "sds.h"
#ifndef __BC_TOOLBOX_H__
#define __BC_TOOLBOX_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_exec_io.h"
#include "edc_error.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_tool;
struct edc_task; /* fwd: the task that issued the tool call; passed to tools via ctx->task */

struct edc_toolbox
{
    struct edc_tool *tools;
    struct edc_config *conf;
    cJSON *schemas;
    /* The memory_* tools' schemas alone. The curation task (SINK_SESSION_MEMORY) gets
     * this subset instead of the full set; folded in edc_toolbox_register by name.
     */
    cJSON *memory_schemas;
    edc_actor_t *scheduler;
    edc_actor_t *channel_actor;
};

struct edc_toolbox_call_ctx
{
    struct edc_toolbox *tb;
    edc_channel_type call_from_type;
    void *request_id; /* the request_id of the task that issued this tool call (injected by the toolbox actor) */
    struct edc_task
        *task; /* the task that issued this tool call (same source as request_id; injected by app when building the request chain; NULL on a non-task path) */
    /* The tool this call was dispatched to (filled in by edc_toolbox_exec, which is the
     * only path that resolves a name to a tool). Lets the actor hand a cancellation back to
     * the tool that owns the in-flight work, instead of the port hardcoding one tool. */
    struct edc_tool *tool;
    void *priv;
};

/* struct edc_toolbox_emit_msg moved to edc_exec_io.h (the executor's I/O vocabulary). */

extern struct edc_toolbox_call_ctx *edc_toolbox_call_ctx_new(struct edc_toolbox *tb, edc_channel_type channel_type,
                                                             void *request_id, struct edc_task *task, void *priv);
extern void edc_toolbox_call_ctx_free(struct edc_toolbox_call_ctx *);
extern void edc_toolbox_emit_msg_free(void *p);
extern struct edc_toolbox *edc_toolbox_new(struct edc_config *conf, edc_actor_t *scheduler, edc_actor_t *channel_actor);
extern void edc_toolbox_free(struct edc_toolbox *tb);

/* Write a capped tool result to a managed file under the workspace and return its absolute
 * path (NULL when it could not be written). This is what makes "truncated" mean "moved, not
 * lost": the caller shows a bounded preview plus this path. Caller frees the result. */
extern sds edc_toolbox_spill_output(struct edc_toolbox *tb, const char *tool_name, const char *session_id,
                                    const char *content, size_t len);

extern void edc_toolbox_register(struct edc_toolbox *tb, struct edc_tool *tool);
extern void edc_toolbox_register_defaults(struct edc_toolbox *tb);
/* The request-shaped schema entry for one tool ({"type":"function","function":{...}}).
 * Exposed because the declaration set is derived elsewhere (edc_toolset); the caller owns
 * the returned object. */
extern cJSON *edc_tool_to_schema(const struct edc_tool *tool);

extern void edc_toolbox_exec(struct edc_toolbox_call_ctx *, const char *name, cJSON *args,
                             void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *));

/* Request a TOOL-type channel message for the tool result (header = short
 * command/tool description, content = tool output). The request is routed
 * through the scheduler actor, which only forwards it when the originating
 * task is still the session's active task — so background tasks stay silent.
 *
 * render_pref is declared explicitly by the sender (the tool) from the output's nature;
 * the renderer does not guess:
 *   - emit_tool_message_plain: plain-text output (code/command output/confirmations),
 *     shown as-is;
 *   - emit_tool_message_md: the output itself is Markdown (a checklist/table), rendered
 *     as Markdown.
 * The message is forwarded through the scheduler and shown only while the source task is
 * still the active task -- background tasks stay silent. */
extern void edc_toolbox_emit_tool_message_plain(struct edc_toolbox_call_ctx *ctx, const char *header,
                                                const char *content);
extern void edc_toolbox_emit_tool_message_md(struct edc_toolbox_call_ctx *ctx, const char *header, const char *content);
#endif
