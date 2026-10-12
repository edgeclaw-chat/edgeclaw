#include "edc_toolbox_actor.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_scheduler.h"
#include "edc_task.h"
#include "edc_toolbox.h"
#include "log.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

static edc_actor_t *g_toolbox_actor = NULL; /* the port provider's registration slot (see the "ports" section at the end) */

/* Report "a tool started executing" to the requester (scheduler): data = strdup(call_id),
 * freed by free. On receipt the scheduler marks that call_id's ledger record EXECUTING --
 * so resume can tell "not executed" (PENDING) from "executing" (EXECUTING) precisely,
 * no longer inferring from chain order. */
static void toolbox_notify_tool_started(edc_actor_t *actor, edc_actor_t *to, void *request_id,
                                        struct edc_toolbox_actor_req *req)
{
    if (!to || !req || !req->call_id)
        return;
    edc_actor_send(to, edc_actor_msg_new(BC_ACTOR_MSG_TOOL_STARTED, actor, request_id, strdup(req->call_id), free));
}

struct edc_toolbox_actor_exec_ctx
{
    struct edc_toolbox_actor_req *req;
    edc_actor_t *actor;
    edc_actor_msg_t *msg;
};

static struct edc_toolbox_actor_exec_ctx *edc_toolbox_actor_exec_ctx_new(edc_actor_t *actor, edc_actor_msg_t *msg,
                                                                         struct edc_toolbox_actor_req *req)
{
    struct edc_toolbox_actor_exec_ctx *ctx = g_new0(struct edc_toolbox_actor_exec_ctx, 1);
    ctx->actor = actor;
    ctx->msg = msg;
    ctx->req = req;
    return ctx;
}

/* ── In-flight tool-call registry ────────────────────────────
 * A tool call is "in flight" from entering TOOL_REQUEST until its last async callback
 * finishes. At teardown the actor loop is uv_stop()ped, so these callbacks never fire and
 * the call context must be released proactively after the actor stops (see
 * edc_toolbox_actor_free). */
static void edc_toolbox_actor_inflight_add(edc_toolbox_actor_state_t *state, struct edc_toolbox_call_ctx *call)
{
    if (state->inflight_count == state->inflight_cap)
    {
        size_t cap = state->inflight_cap ? state->inflight_cap * 2 : 4;
        struct edc_toolbox_call_ctx **grown = realloc(state->inflight, cap * sizeof(*grown));
        if (!grown)
            return; /* registration failed: fall back to the old "leak on exit" behaviour; not fatal */
        state->inflight = grown;
        state->inflight_cap = cap;
    }
    state->inflight[state->inflight_count++] = call;
}

static void edc_toolbox_actor_inflight_remove(edc_toolbox_actor_state_t *state, struct edc_toolbox_call_ctx *call)
{
    for (size_t i = 0; i < state->inflight_count; i++)
    {
        if (state->inflight[i] == call)
        {
            state->inflight[i] = state->inflight[--state->inflight_count];
            return;
        }
    }
}

/* Fully release one in-flight tool call: the unsent req chain + actor msg + exec ctx +
 * call ctx. Ownership: exec->msg->data may point at the first, not-yet-sent req (the same
 * object as exec->req), or at a previous req already handed to the consumer (mid-chain).
 * Detach msg->data first so msg does not double-free a handed-off req; the reqs this call
 * still holds are released from exec->req along ->next. */
static void edc_toolbox_actor_inflight_free(struct edc_toolbox_call_ctx *call)
{
    if (!call)
        return;

    struct edc_toolbox_actor_exec_ctx *exec = call->priv;
    if (exec)
    {
        struct edc_toolbox_actor_req *req = exec->req;
        if (exec->msg)
        {
            exec->msg->data = NULL;
            edc_actor_msg_free(exec->msg);
        }
        while (req)
        {
            struct edc_toolbox_actor_req *next = req->next;
            edc_toolbox_actor_req_free(req);
            req = next;
        }
        free(exec);
    }
    edc_toolbox_call_ctx_free(call);
}

static char *_HINT = "\n\n[Analyze the error above and try a different approach.]";

// TODO: fix error handle
static void edc_toolbox_actor_callback(struct edc_toolbox_call_ctx *tb_ctx, sds result, EdcError *error)
{
    if (error)
    {
        if (error->message)
        {
            log_error("DEBUG actor_callback: error->message='%s', error->code=%d, error ptr=%p, sdslen=%zu",
                      error->message, error->code, (void *)error, sdslen(error->message));
            result = sdsnew(error->message);
        }
        else
        {
            log_error("DEBUG actor_callback: error->message=NULL, error->code=%d, error ptr=%p", error->code,
                      (void *)error);
            result = sdsnew("unknown error");
        }
        log_error("DEBUG actor_callback: after sdsnew, result='%s', len=%zu", result, sdslen(result));
        result = sdscat(result, _HINT);
        edc_error_free(error);
    }

    if (!result)
    {
        result = sdsnew("");
    }

    if (!tb_ctx)
    {
        sdsfree(result);
        return;
    }

    struct edc_toolbox_actor_exec_ctx *ctx = tb_ctx->priv;
    struct edc_toolbox_actor_req *req = ctx->req;
    edc_actor_msg_t *msg = ctx->msg;
    edc_actor_t *actor = ctx->actor;
    edc_toolbox_actor_state_t *state = (edc_toolbox_actor_state_t *)actor->state;

    char *args = cJSON_PrintUnformatted(req->args);
    log_debug("tool call request name = %s, args = %s, result_len = %zu", req->name, args, sdslen(result));
    cJSON_free(args);

    req->result = result;
    if ((sdslen(result)) > state->toolbox->conf->tool_max_output_size)
    {
        size_t len = sdslen(result);
        sdsrange(result, 0, state->toolbox->conf->tool_max_output_size);
        result = sdscatprintf(result, "\n\n[Result truncated to 10000 characters, total length: %zu]", len);
        req->result = result;
    }

    struct edc_toolbox_actor_req *next = req->next;
    edc_actor_send(msg->from, edc_actor_msg_new(BC_ACTOR_MSG_TOOL_RESPONSE, actor, msg->request_id, req,
                                                (edc_actor_data_free_fn)edc_toolbox_actor_req_free));

    if (next)
    {
        req = next;
        ctx->req = req;
        toolbox_notify_tool_started(actor, msg->from, msg->request_id, req);
        edc_toolbox_exec(tb_ctx, req->name, req->args, edc_toolbox_actor_callback);
    }
    else
    {
        /* Chain tail: the current req was handed to the consumer above, so do not use
         * inflight_free here (it would free exec->req). Just deregister and free the ctx
         * itself. */
        msg->data = NULL;
        edc_toolbox_actor_inflight_remove(state, tb_ctx);
        edc_toolbox_call_ctx_free(tb_ctx);
        edc_actor_msg_free(msg);
        free(ctx);
    }
}

// TODO: async exec
static void edc_toolbox_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_toolbox_actor_state_t *state = (edc_toolbox_actor_state_t *)actor->state;
    struct edc_toolbox_actor_req *req = (struct edc_toolbox_actor_req *)msg->data;

    switch (msg->type)
    {
    case BC_ACTOR_MSG_TOOL_REQUEST: {
        struct edc_toolbox_call_ctx *ctx =
            edc_toolbox_call_ctx_new(state->toolbox, req->call_from_type, msg->request_id, req->task,
                                     edc_toolbox_actor_exec_ctx_new(actor, msg, req));
        if (ctx)
            edc_toolbox_actor_inflight_add(state, ctx);
        toolbox_notify_tool_started(actor, msg->from, msg->request_id, req);
        edc_toolbox_exec(ctx, req->name, req->args, edc_toolbox_actor_callback);
        break;
    }
    default:
        edc_actor_msg_free(msg);
        break;
    }
}

void edc_toolbox_actor_free(edc_actor_t *actor)
{
    edc_toolbox_actor_state_t *state = (edc_toolbox_actor_state_t *)actor->state;
    if (g_toolbox_actor == actor) /* the port provider's registration slot: leave no dangling pointer */
        g_toolbox_actor = NULL;
    /* The runner has stopped; in-flight tool callbacks never fire -- reclaim proactively, or they leak on process exit */
    for (size_t i = 0; i < state->inflight_count; i++)
        edc_toolbox_actor_inflight_free(state->inflight[i]);
    free(state->inflight);
    free(state);
    edc_actor_free(actor);
}

void edc_toolbox_actor_req_free(struct edc_toolbox_actor_req *req)
{
    if (!req)
        return;

    if (req->name)
    {
        sdsfree(req->name);
    }

    if (req->call_id)
    {
        sdsfree(req->call_id);
    }
    if (req->args)
    {

        cJSON_Delete(req->args);
    }
    if (req->result)
    {
        sdsfree(req->result);
    }
    free(req);
}

struct edc_toolbox_actor_req *edc_toolbox_actor_req_new(sds name, cJSON *args, sds call_id,
                                                        edc_channel_type call_from_type, struct edc_task *task)
{
    struct edc_toolbox_actor_req *req = g_new0(struct edc_toolbox_actor_req, 1);
    req->name = name;
    req->args = args;
    req->call_id = call_id;
    req->next = NULL;
    req->call_from_type = call_from_type;
    req->task = task;
    return req;
}

edc_actor_t *edc_toolbox_actor_new(struct edc_toolbox *toolbox)
{
    edc_toolbox_actor_state_t *state = g_new0(edc_toolbox_actor_state_t, 1);
    state->toolbox = toolbox;

    return edc_actor_new("edc_toolbox_actor", edc_toolbox_actor_handler, state);
}

/* ── Request side: post a request chain to the toolbox actor ──
 * The single posting exit: dispatch and redispatch both go through here, so
 * BC_ACTOR_MSG_TOOL_REQUEST appears exactly twice in the file (here + the actor's own
 * handler). */
/* One request chain, one post. Note data_free_fn is a PER-NODE freer: normally each node
 * is taken away by its own reply message, but if this chain is dropped in the mailbox
 * (teardown draining it), only the chain head is freed => the tail leaks. Decided
 * (2026-10-09) NOT to fix (it only happens on the process-teardown drain path, and the
 * process exits right after); recorded here so it is not reported as a new bug. Real fix:
 * add a req_chain_free walking ->next and use it here. */
static void edc_toolbox_actor_send_chain(edc_actor_t *actor, struct edc_scheduler *sched, void *request_id,
                                         struct edc_toolbox_actor_req *chain)
{
    edc_actor_send(actor, edc_actor_msg_new(BC_ACTOR_MSG_TOOL_REQUEST, edc_scheduler_get_actor(sched), request_id,
                                            chain, (edc_actor_data_free_fn)edc_toolbox_actor_req_free));
}

/* Assemble the LLM response's tool_calls into a request chain and post it (the head is a
 * dummy; an empty chain is not posted). Only FUNCTION calls are handled; ownership of
 * name/args/id moves to the req (the response side no longer references them). */
void edc_toolbox_actor_dispatch(edc_actor_t *actor, struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                void *request_id)
{
    if (!actor || !task || !llm_resp)
        return;

    struct edc_llm_actor_resp *resp = llm_resp;

    /* Thought echo: putting content on screen is the CHANNEL's job, so it still goes
     * through the scheduler's send_reply port -- this module does not know the channel
     * handle (rule: take only what you can get). */
    if ((resp->reasoning_content && sdslen(resp->reasoning_content) > 0) ||
        (resp->content && sdslen(resp->content) > 0))
    {
        sds content = (resp->content && sdslen(resp->content) > 0) ? resp->content : resp->reasoning_content;
        sds text = sdscatprintf(sdsempty(), "%s", content);
        if (sched && sched->send_reply)
            sched->send_reply(sched, task, text, EDC_CHANNEL_MESSAGE_THOUGHT_TYPE, request_id);
        sdsfree(text);
    }

    if (!resp->tool_calls)
        return;

    struct edc_toolbox_actor_req *head = edc_toolbox_actor_req_new(NULL, NULL, NULL, task->channel_type, task);
    struct edc_toolbox_actor_req *req = head;

    for (unsigned i = 0; i < utarray_len(resp->tool_calls); i++)
    {
        struct edc_llm_chat_tool_call *tool_call = (struct edc_llm_chat_tool_call *)utarray_eltptr(resp->tool_calls, i);
        if (tool_call->type == BC_LLM_TOOL_CALL_TYPE_FUNCTION)
        {
            req->next = edc_toolbox_actor_req_new(tool_call->function.name, tool_call->function.arguments,
                                                  tool_call->id, task->channel_type, task);
            req = req->next;
            tool_call->function.arguments = NULL;
            tool_call->function.name = NULL;
            tool_call->id = NULL;
        }
    }

    req = head->next;
    edc_toolbox_actor_req_free(head);
    if (req)
        edc_toolbox_actor_send_chain(actor, sched, request_id, req);
}

/* resume: rebuild the toolbox request chain from the task's tool ledger for records still
 * PENDING (not executed) and post it. EXECUTING records get their interrupt reply from the
 * scheduler during resume and are not included. Returns the number posted. */
int edc_toolbox_actor_redispatch(edc_actor_t *actor, struct edc_scheduler *sched, struct edc_task *task,
                                 void *request_id)
{
    if (!actor || !task)
        return 0;

    /* dummy head, matching the dispatch path (an empty req is not chained) */
    struct edc_toolbox_actor_req *head = edc_toolbox_actor_req_new(NULL, NULL, NULL, task->channel_type, task);
    struct edc_toolbox_actor_req *tail = head;
    int count = 0;

    struct edc_tool_call_record *rec;
    for (rec = task->tool_calls; rec; rec = rec->next)
    {
        if (rec->status != TOOL_CALL_PENDING)
            continue;
        tail->next = edc_toolbox_actor_req_new(rec->name ? sdsdup(rec->name) : sdsempty(),
                                               rec->args ? cJSON_Duplicate(rec->args, 1) : NULL,
                                               rec->call_id ? sdsdup(rec->call_id) : NULL, task->channel_type, task);
        tail = tail->next;
        count++;
    }

    struct edc_toolbox_actor_req *chain = head->next;
    edc_toolbox_actor_req_free(head);
    if (!chain)
        return 0;

    edc_toolbox_actor_send_chain(actor, sched, request_id, chain);
    return count;
}

/* ── Ports: the toolbox provides the two tool ports ─────────
 *
 * The tool protocol (req chain / dispatch / rebuild from the ledger) lives in this file,
 * so "install the ports onto the scheduler" is here too -- the assembly root calls it
 * once at assembly time and never appears on a runtime path again. Port signatures are
 * unchanged; kernel call sites and test fakes are unaffected.
 *
 * The toolbox actor is THE ONLY ONE in the process, so the provider just registers itself
 * at bind (the same reason as the channel: no boundary handle hung on the kernel's
 * scheduler); the registration slot is at the top of the file. */

static void edc_toolbox_cb_dispatch(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                    void *request_id)
{
    edc_toolbox_actor_dispatch(g_toolbox_actor, sched, task, llm_resp, request_id);
}

static int edc_toolbox_cb_redispatch(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    return edc_toolbox_actor_redispatch(g_toolbox_actor, sched, task, request_id);
}

void edc_toolbox_actor_bind_scheduler(edc_actor_t *actor, struct edc_scheduler *sched)
{
    if (!actor || !sched)
        return;

    g_toolbox_actor = actor;
    sched->dispatch_tools = edc_toolbox_cb_dispatch;
    sched->redispatch_tools = edc_toolbox_cb_redispatch;
}
