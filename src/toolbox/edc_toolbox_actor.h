#include "sds.h"
#ifndef __BC_LLM_TOOLBOX_ACTOR_H__
#define __BC_LLM_TOOLBOX_ACTOR_H__

#include "edc_actor.h"
#include "edc_toolbox.h"

typedef struct
{
    struct edc_toolbox *toolbox;
    /* Tool calls still executing (their async callback has not fired). At teardown the
     * actor loop is stopped outright, so these callbacks never run again; without
     * registering them for release here, the call contexts (and the req chain / actor msg
     * they hold) would leak on process exit. */
    struct edc_toolbox_call_ctx **inflight;
    size_t inflight_count;
    size_t inflight_cap;
} edc_toolbox_actor_state_t;

struct edc_task; /* fwd */

/* struct edc_toolbox_actor_req moved to edc_exec_io.h (the executor's I/O vocabulary). */

extern edc_actor_t *edc_toolbox_actor_new(struct edc_toolbox *toolbox);
extern void edc_toolbox_actor_free(edc_actor_t *actor);
extern struct edc_toolbox_actor_req *edc_toolbox_actor_req_new(sds name, cJSON *args, sds call_id,
                                                               edc_channel_type call_from_type, struct edc_task *task);
extern void edc_toolbox_actor_req_free(struct edc_toolbox_actor_req *req);

/* ── Request side: "what the LLM returned" -> toolbox request chain ──
 *
 * The response side is the actor's own handler (BC_ACTOR_MSG_TOOL_REQUEST -> execute ->
 * send the result back); this is the same protocol's REQUEST direction -- the req chain's
 * shape, the assembly of BC_ACTOR_MSG_TOOL_REQUEST, and "rebuild the not-yet-executed
 * requests from the task ledger" (resume) all belong here, so they live with the actor.
 *
 * The caller (scheduler / executor) only declares "a tool is due / a redispatch is due".
 * Thought echo still goes through the sched->send_reply port -- putting content on screen
 * is the CHANNEL's job.
 * `redispatch` returns the number of requests rebuilt and posted (0 = nothing to do). */
struct edc_scheduler; /* fwd */
extern void edc_toolbox_actor_dispatch(edc_actor_t *actor, struct edc_scheduler *sched, struct edc_task *task,
                                       void *llm_resp, void *request_id);
extern int edc_toolbox_actor_redispatch(edc_actor_t *actor, struct edc_scheduler *sched, struct edc_task *task,
                                        void *request_id);

/* ── Ports: the toolbox provides the two tool ports ─────────
 *
 * The tool protocol (req chain / dispatch / rebuild from the ledger) all lives in this
 * file, so "install the ports onto the scheduler" is here too -- the provider wires it.
 * The assembly root calls it once at assembly time and never appears on a runtime path
 * again (dispatch_tools / redispatch_tools).
 * Port signatures are unchanged: kernel call sites and test fakes are unaffected. */
extern void edc_toolbox_actor_bind_scheduler(edc_actor_t *actor, struct edc_scheduler *sched);
#endif
