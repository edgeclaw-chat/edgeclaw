#include "edc_toolbox_actor.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_toolbox.h"
#include "log.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

/* 向请求方 (scheduler) 上报"某个工具开始执行": data = strdup(call_id), 由 free
 * 释放。scheduler 收到后把该 call_id 的账本记录标为 EXECUTING —— 这样 resume
 * 时能精确区分"未执行"(PENDING) 与"执行中"(EXECUTING), 不再依赖链序推断。 */
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

    if (!tb_ctx || !tb_ctx->priv)
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
        msg->data = NULL;
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
        struct edc_toolbox_call_ctx *ctx = edc_toolbox_call_ctx_new(
            state->toolbox, req->call_from_type, msg->request_id, req->task,
            edc_toolbox_actor_exec_ctx_new(actor, msg, req));
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
