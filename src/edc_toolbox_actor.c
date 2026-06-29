#include "edc_string.h"
#include "edc_toolbox_actor.h"
#include "edc_actor.h"
#include "edc_toolbox.h"
#include "log.h"

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
static void edc_toolbox_actor_callback(struct edc_toolbox_call_ctx *tb_ctx, sds result, GError *error)
{
    if (error)
    {
        result = edc_str_new(error->message);
        edc_str_cat(result, _HINT);
        g_error_free(error);
    }

    if (!result)
    {
        result = edc_str_new("");
    }

    struct edc_toolbox_actor_exec_ctx *ctx = tb_ctx->priv;
    struct edc_toolbox_actor_req *req = ctx->req;
    edc_actor_msg_t *msg = ctx->msg;
    edc_actor_t *actor = ctx->actor;
    edc_toolbox_actor_state_t *state = (edc_toolbox_actor_state_t *)actor->state;

    char *args = cJSON_PrintUnformatted(req->args);
    log_debug("tool call request name = %s, args = %s, result_len = %zu", req->name, args, edc_str_len(result));
    free(args);

    req->result = result;
    if ((edc_str_len(result)) > state->toolbox->conf->tool_max_output_size)
    {
        size_t len = edc_str_len(result);
        edc_str_trunc(result, state->toolbox->conf->tool_max_output_size);
        edc_str_printf(result, "\n\n[Result truncated to 10000 characters, total length: %zu]", len);
        req->result = result;
    }

    struct edc_toolbox_actor_req *next = req->next;
    edc_actor_send(msg->from, edc_actor_msg_new(BC_ACTOR_MSG_TOOL_RESPONSE, actor, msg->request_id, req,
                                              (edc_actor_data_free_fn)edc_toolbox_actor_req_free));

    if (next)
    {
        req = next;
        ctx->req = req;
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
        struct edc_toolbox_call_ctx *ctx =
            edc_toolbox_call_ctx_new(state->toolbox, req->call_from, edc_toolbox_actor_exec_ctx_new(actor, msg, req));
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
        edc_str_free(req->name);
    }

    if (req->call_id)
    {
        edc_str_free(req->call_id);
    }
    if (req->args)
    {

        cJSON_Delete(req->args);
    }
    if (req->result)
    {
        edc_str_free(req->result);
    }
    free(req);
}

struct edc_toolbox_actor_req *edc_toolbox_actor_req_new(sds name, cJSON *args, sds call_id,
                                                      struct edc_channel *call_from)
{
    struct edc_toolbox_actor_req *req = g_new0(struct edc_toolbox_actor_req, 1);
    req->name = name;
    req->args = args;
    req->call_id = call_id;
    req->next = NULL;
    req->call_from = call_from;
    return req;
}

edc_actor_t *edc_toolbox_actor_new(struct edc_toolbox *toolbox)
{
    edc_toolbox_actor_state_t *state = g_new0(edc_toolbox_actor_state_t, 1);
    state->toolbox = toolbox;

    return edc_actor_new("edc_toolbox_actor", edc_toolbox_actor_handler, state);
}
