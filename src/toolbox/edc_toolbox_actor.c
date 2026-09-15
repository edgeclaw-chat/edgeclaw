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

static edc_actor_t *g_toolbox_actor = NULL; /* 端口实现者的登记位 (见文件末"端口"一节) */

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

/* ── 在途工具调用登记 ─────────────────────────────────────────
 * 一次工具调用从 TOOL_REQUEST 进入到最后一个异步回调跑完为止都是"在途"。
 * actor loop 在 teardown 时被 uv_stop, 这些回调不会再触发, 所以调用上下文
 * 必须在 actor 停止后被主动释放 (见 edc_toolbox_actor_free)。 */
static void edc_toolbox_actor_inflight_add(edc_toolbox_actor_state_t *state, struct edc_toolbox_call_ctx *call)
{
    if (state->inflight_count == state->inflight_cap)
    {
        size_t cap = state->inflight_cap ? state->inflight_cap * 2 : 4;
        struct edc_toolbox_call_ctx **grown = realloc(state->inflight, cap * sizeof(*grown));
        if (!grown)
            return; /* 登记失败: 回退到旧的"退出即漏"行为, 不致命 */
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

/* 彻底释放一次在途工具调用: 未发出的 req 链 + actor msg + exec ctx + 调用 ctx。
 * 所有权说明: exec->msg->data 可能指向"尚未发出"的首个 req (与 exec->req 同一
 * 对象), 也可能指向"已转交消费者"的前一 req (链中途)。先把 msg->data 摘掉,
 * 保证 msg 不会去 double-free 已转交的 req; 本调用仍持有的 req 从 exec->req
 * 起沿 ->next 逐个释放。 */
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
        /* 链尾: 当前 req 已在上面转交给消费者, 这里不能再用 inflight_free
         * (它会去 free exec->req)。只需注销登记并释放 ctx 自身。 */
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
    if (g_toolbox_actor == actor) /* 端口实现者的登记位: 别留野指针 */
        g_toolbox_actor = NULL;
    /* runner 已 stop, 在途工具回调不会再触发 —— 主动回收, 否则随进程退出泄漏 */
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

/* ── 请求侧: 投一条请求链给 toolbox actor ─────────────────────
 * 唯一的投递出口: dispatch 与 redispatch 都从这里出去, 所以
 * `BC_ACTOR_MSG_TOOL_REQUEST` 全文件只出现两次 (这里 + actor 自己的 handler)。 */
static void edc_toolbox_actor_send_chain(edc_actor_t *actor, struct edc_scheduler *sched, void *request_id,
                                         struct edc_toolbox_actor_req *chain)
{
    edc_actor_send(actor, edc_actor_msg_new(BC_ACTOR_MSG_TOOL_REQUEST, edc_scheduler_get_actor(sched), request_id,
                                            chain, (edc_actor_data_free_fn)edc_toolbox_actor_req_free));
}

/* 把 LLM 响应里的 tool_calls 拼成一条请求链并投递 (链首是 dummy head, 空链不投)。
 * 只处理 FUNCTION 类型的 call; name/args/id 的所有权转移给 req (响应侧不再引用)。 */
void edc_toolbox_actor_dispatch(edc_actor_t *actor, struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                void *request_id)
{
    if (!actor || !task || !llm_resp)
        return;

    struct edc_llm_actor_resp *resp = llm_resp;

    /* 思维内容回显: 内容上屏是**通道**的活, 所以仍走 scheduler 的 send_reply
     * 端口 —— 本模块不认识通道句柄 (规则: 能拿到的才拿)。 */
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

/* resume: 把 task 工具账本里仍为 PENDING (尚未执行) 的记录重建 toolbox 请求链
 * 并投递。EXECUTING 的记录由 scheduler 在 resume 时补中断响应, 不在此列。
 * 返回投递的条数。 */
int edc_toolbox_actor_redispatch(edc_actor_t *actor, struct edc_scheduler *sched, struct edc_task *task,
                                 void *request_id)
{
    if (!actor || !task)
        return 0;

    /* dummy head, 与原 dispatch 路径一致 (空 req 不入链) */
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

/* ── 端口: toolbox 是两条工具端口的提供方 ────────────────────
 *
 * 工具协议 (req 链 / dispatch / 按账本重建) 都住本文件, 所以"把端口装到
 * scheduler 上"也归这里 —— 装配根只在装配期叫一次, 运行期不再出现在路径上。
 * 端口签名一个字没动, kernel 的调用点与测试里的假端口都不受影响。
 *
 * toolbox actor 是**进程里唯一的一个**, 所以实现者在 bind 时登记自己即可
 * (与通道同一个理由: 不往 kernel 的 scheduler 上挂边界句柄); 登记位在文件开头。 */

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
