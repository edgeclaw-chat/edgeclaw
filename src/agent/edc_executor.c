/* edc_executor.c —— task 之内的推进器。见 edc_executor.h 与 docs/runtime-model.md §2。
 *
 * P1：本文件**原样搬来** sched_handle_llm_resp / sched_handle_tool_resp /
 * sched_handle_tool_started / sched_stop_at_iteration_cap 的**决策**部分，
 * 副作用改为经由 ops 出站。**行为不变**（搬迁纪律：逐行核对）。
 */
#include "edc_executor.h"
#include "edc_config.h"
#include "edc_llm_actor.h"
#include "edc_session.h"
#include "edc_task_group.h"
#include "edc_toolbox.h"
#include "edc_toolbox_actor.h"
#include "edc_turn.h"
#include "sds.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

void edc_executor_init(struct edc_executor *ex, const struct edc_executor_ops *ops, void *ctx)
{
    ex->ops = ops;
    ex->ctx = ctx;
}

/* ── EXEC_DONE：LLM 响应到达 ───────────────────────────────── */

static edc_step_outcome exec_done(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const struct edc_executor_ops *ops = ex->ops;
    struct edc_event_exec *e = &ev->as.exec;
    void *req_id = (void *)ev->request_id;

    /* Token 账: 把本次响应的 usage 累加到 task。放在分支之前, 让 STOP、
     * TOOL_CALLING 以及 error 三条路径都计入 —— 一次用户输入常触发多轮
     * 请求 (工具链续发), 每轮都要算, 否则会漏掉中间轮。 */
    edc_task_add_usage(task, e->prompt_tokens, e->completion_tokens, e->total_tokens, e->cached_tokens);

    if (e->failed)
    {
        if (ops->unregister_task)
            ops->unregister_task(ex->ctx, req_id);
        task->current_request_id = NULL;
        if (ops->set_task_state)
            ops->set_task_state(ex->ctx, task, EDC_TASKSTEP_DONE, EDC_LIFE_ERROR);
        if (e->content)
            edc_task_set_output(task, sdsnew(e->content));
        if (ops->flush_redirects)
            ops->flush_redirects(ex->ctx, task);
        if (ops->send_reply)
        {
            const char *err = e->error_message ? e->error_message : "LLM request failed";
            bool no_key = ops->no_api_key && ops->no_api_key(ex->ctx);
            if (no_key)
            {
                /* First run: with no API key the request can only fail, so
                 * point the user at the /connect dialog instead of just
                 * reporting the error. */
                sds combined = sdscatprintf(
                    sdsempty(), "%s\n\nNo API key configured — run /connect to set the API URL, model and key.", err);
                ops->send_reply(ex->ctx, task, combined, EDC_CHANNEL_MESSAGE_SYS_TYPE, req_id);
                sdsfree(combined);
            }
            else
            {
                ops->send_reply(ex->ctx, task, err, EDC_CHANNEL_MESSAGE_SYS_TYPE, req_id);
            }
        }
        return EDC_STEP_ERROR;
    }

    if (e->reason == EDC_REASON_TOOL_CALL)
    {
        if (ops->set_task_state)
            ops->set_task_state(ex->ctx, task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
        if (task->active_turn)
        {
            /* 所有权转入 turn；被接手才消掉源指针（与今天逐字一致）。 */
            edc_turn_add_assistant_message(task->active_turn, e->msg);
            e->msg = NULL;
        }
        /* 账本: 先把本轮 tool_calls 全部登记为 PENDING (dispatch 会转移
         * name/args/id 的所有权, 故必须先登记), 派发后再把链首提升 EXECUTING
         * —— 中断后 resume 据此判定哪些工具需要重新执行。 */
        for (int i = 0; i < e->n_calls; i++)
        {
            const struct edc_tool_call *tc = &e->calls[i];
            edc_task_tool_call_add(task, tc->id, tc->name, tc->args);
        }
        /* keep task registered — tool responses use same request_id */
        if (ops->dispatch_tools)
            ops->dispatch_tools(ex->ctx, task, e->tool_calls_raw, req_id);
        return EDC_STEP_BLOCKED;
    }

    if (e->reason == EDC_REASON_STOP)
    {
        if (ops->unregister_task)
            ops->unregister_task(ex->ctx, req_id);
        task->current_request_id = NULL;

        /* 属主 session: 后台 session 里的任务完成时, turn 与 completion_seq 必须
         * 落回它自己, 不能用当前绑定的 session。 */
        struct edc_session *own = ops->task_session ? ops->task_session(ex->ctx, task) : NULL;
        if (task->active_turn)
        {
            /* 最终回复就是渲染端按 Markdown 显示的那条 (send_reply 走 RESP) ——
             * 把偏好写进消息本身。 */
            if (e->msg)
                cJSON_ReplaceItemInObjectCaseSensitive(e->msg, "render", cJSON_CreateString("markdown"));
            edc_turn_add_assistant_message(task->active_turn, e->msg);
            e->msg = NULL;

            /* sync turn messages to group history (context sharing)。
             * sink=GROUP_MEMORY (system task) 除外: 它的 turn 是折叠 prompt 与
             * 摘要, 不属于任何"对话历史" —— 混进去的话, 同一个系统组里**下一次**
             * 折叠就会看到上一次的 prompt 与答案。 */
            if (task->sink != EDC_SINK_GROUP_MEMORY)
            {
                for (size_t qi = task->active_turn->messages.head; qi < task->active_turn->messages.tail; qi++)
                {
                    cJSON *item = task->active_turn->messages.data[qi];
                    edc_task_group_history_add_ref(task->group, item);
                }
            }
        }
        edc_task_commit_active_turn(task);
        if (e->content)
            edc_task_set_output(task, sdsnew(e->content));
        if (ops->flush_redirects)
            ops->flush_redirects(ex->ctx, task);
        /* completion_seq = session 的**完成顺序** (折叠水位、$N 输出引用都读它)。
         * sink=GROUP_MEMORY 的 system task 不进这个序列: 它不是用户的完成次序里
         * 的一环 (规则 ④ "不进入被折叠 group 的 completion_seq" 的落地 —— 连它所在
         * session 的计数器也不动, 于是折叠不改变任何用户可见的序号)。 */
        if (own && task->sink != EDC_SINK_GROUP_MEMORY)
            task->completion_seq = own->next_completion_seq++;
        else
            task->completion_seq = 0;
        if (ops->set_task_state)
            ops->set_task_state(ex->ctx, task, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);

        /* session 消息数刚因 turn 提交而增长。被更新输入顶替、或在后台/
         * 依赖链中完成的任务其 turn 是"静默"的, 需要主动通知一次,
         * 否则侧边栏的 session 统计会停在旧值。 */
        if (ops->task_snapshot)
            ops->task_snapshot(ex->ctx, task);

        if (ops->notify_dependents)
            ops->notify_dependents(ex->ctx, task);

        /* auto-trigger dependents with queued input */
        if (ops->start_queued_dependents)
            ops->start_queued_dependents(ex->ctx, task);

        /* 产出送到哪: 由 sink 决定 (runtime-model.md §1.4 / §5 规则 7 —— 分流这一处
         * 在 executor, 但"镜像装配读 sink"另有一处且合法)。CHANNEL = 回给用户
         * (send_reply 内部还会查 active_task 闸门);
         * GROUP_MEMORY = 交给上层写进目标组的记忆 (fold), 不产生任何回话。 */
        if (task->sink == EDC_SINK_GROUP_MEMORY)
        {
            if (ops->deliver_group_memory)
                ops->deliver_group_memory(ex->ctx, task, e->content);
        }
        else if (ops->send_reply)
        {
            ops->send_reply(ex->ctx, task, e->content, EDC_CHANNEL_MESSAGE_RESP_TYPE, req_id);
        }

        /* auto-reap disabled: tasks stay COMPLETED for cross-group join */

        /* 触发 L1 折叠检查 (task 完成后可能达到 group fold 阈值) */
        if (ops->trigger_memory)
            ops->trigger_memory(ex->ctx);

        if (ops->save_session)
            ops->save_session(ex->ctx, own);
        return EDC_STEP_DONE;
    }

    /* 未知 finish reason：与今天一致 —— ERROR + 系统消息。 */
    if (ops->unregister_task)
        ops->unregister_task(ex->ctx, req_id);
    task->current_request_id = NULL;
    if (ops->set_task_state)
        ops->set_task_state(ex->ctx, task, EDC_TASKSTEP_DONE, EDC_LIFE_ERROR);
    if (ops->send_reply)
        ops->send_reply(ex->ctx, task, "Internal error: unknown finish reason", EDC_CHANNEL_MESSAGE_SYS_TYPE, req_id);
    return EDC_STEP_ERROR;
}

/* ── 到达 max_iter 上限 ──────────────────────────────────────
 * 构造一条 STOP 语义的事件 (内容为收尾说明), 复用 exec_done 的完整终结
 * 路径 —— turn 整体入库、任务置 COMPLETED、通知依赖者、触发 L1 折叠与
 * save_session, 保证被打断的上下文仍是连贯的。
 * （今天这步是伪造一个 edc_llm_actor_resp；归一化之后它就是一个事件。） */
static void stop_at_iteration_cap(struct edc_executor *ex, struct edc_task *task, const char *request_id)
{
    int max_iter = ex->ops->max_iter ? ex->ops->max_iter(ex->ctx) : 0;

    sds notice = sdscatprintf(
        sdsempty(),
        "[stopped: reached max_iter=%d LLM/tool rounds without a final answer — iteration budget exhausted. "
        "Send a follow-up message to continue.]",
        max_iter);

    cJSON *assistant = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "content", notice);

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_EXEC_DONE;
    ev.request_id = request_id;
    ev.as.exec.reason = EDC_REASON_STOP;
    ev.as.exec.content = notice;  /* STOP 分支用 sdsnew 拷贝, 不持有 */
    ev.as.exec.msg = assistant;   /* 所有权转入 active_turn (与真实响应一致) */

    (void)edc_executor_advance(ex, task, &ev);

    sdsfree(notice);
}

/* ── TOOL_DONE：某个工具完成 ───────────────────────────────── */

static edc_step_outcome tool_done(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const struct edc_executor_ops *ops = ex->ops;
    const struct edc_event_tool_done *t = &ev->as.tool_done;

    /* 添加 tool 消息到 active turn */
    if (task->active_turn && t->result)
        edc_turn_add_tool_message(task->active_turn, t->call_id, t->name, t->result);

    /* 账本: 该工具已完成 → 删除该条记录 */
    edc_task_tool_call_remove(task, t->call_id);

    /* 若 tool chain 完成，发送续 LLM 请求 */
    if (!t->has_next)
    {
        /* max_iter 治理: 工具链结束后若已耗尽本轮 LLM 迭代预算, 不再续发
         * LLM。max_iter <= 0 表示不限制。 */
        int max_iter = ops->max_iter ? ops->max_iter(ex->ctx) : 0;
        if (max_iter > 0 && task->llm_rounds >= max_iter)
        {
            stop_at_iteration_cap(ex, task, ev->request_id);
            return EDC_STEP_ERROR; /* 已转入终结路径 */
        }

        if (ops->set_task_state)
            ops->set_task_state(ex->ctx, task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
        if (ops->send_llm)
        {
            task->llm_rounds++; /* 工具链结束后续发 → 再消耗一轮预算 */
            ops->send_llm(ex->ctx, task, (void *)ev->request_id);
        }
        return EDC_STEP_BLOCKED;
    }
    return EDC_STEP_BLOCKED;
}

/* ── TOOL_STARTED：某个工具真正开始执行 ────────────────────── */

static edc_step_outcome tool_started(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const char *call_id = ev->as.call_id;
    if (call_id)
        edc_task_tool_call_mark_executing(task, call_id);
    return EDC_STEP_BLOCKED;
}

/* ── SIGNAL：取消/中断信号 ───────────────────────────────────
 * 今天这三步都在 edc_scheduler_send_signal_to_task_ptr 里，逐字搬过来：
 * 先反注册（让迟到的 LLM/工具响应找不到 task，从而无害），再交给 task 层
 * 改状态，最后按需要落盘。 */
static edc_step_outcome exec_signal(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const struct edc_executor_ops *ops = ex->ops;
    edc_task_signal sig = ev->as.signal.sig;

    /* 1. 反注册 in-flight 请求 —— 迟到的响应必须无害（规则 2）。 */
    if (task->current_request_id && ops->unregister_task)
        ops->unregister_task(ex->ctx, task->current_request_id);

    /* 2. 信号**直接**改 task 状态，不依赖任何设备回执（规则 1）。 */
    edc_task_send_signal(task, sig);

    /* 3. 显式取消必须立即持久化 + 推送快照: CANCELLED 只在内存里, 若不保存,
     *    重启后任务会按 session.json 里的旧状态恢复, 取消看起来像没生效。 */
    if (ev->as.signal.persist && edc_task_is_cancelled(task))
    {
        if (ops->notify_foreground_state)
            ops->notify_foreground_state(ex->ctx, task);
        if (ops->save_session)
            ops->save_session(ex->ctx, ops->task_session ? ops->task_session(ex->ctx, task) : NULL);
    }

    return edc_task_is_cancelled(task) ? EDC_STEP_DONE : EDC_STEP_BLOCKED;
}

/* ── 步进 ──────────────────────────────────────────────────── */

edc_step_outcome edc_executor_advance(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    if (!ex || !task || !ev)
        return EDC_STEP_BLOCKED;

    switch (ev->kind)
    {
    case EDC_EVENT_EXEC_DONE:
        return exec_done(ex, task, ev);
    case EDC_EVENT_TOOL_DONE:
        return tool_done(ex, task, ev);
    case EDC_EVENT_TOOL_STARTED:
        return tool_started(ex, task, ev);
    case EDC_EVENT_SIGNAL:
        return exec_signal(ex, task, ev);
    }
    return EDC_STEP_BLOCKED;
}

/* ── 边界适配（本文件里唯一认识 actor 负载的地方）────────────
 * 负载以 void* 传入 —— 适配是**类型无关的调用约定**，scheduler 侧连结构体
 * 名字都不必出现（§8 验收判据 3）。 */

edc_step_outcome edc_executor_advance_llm_resp(struct edc_executor *ex, struct edc_task *task, void *request_id,
                                               void *resp)
{
    struct edc_llm_actor_resp *r = resp;
    if (!r)
        return EDC_STEP_BLOCKED;

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_EXEC_DONE;
    ev.request_id = request_id;
    ev.as.exec.failed = (r->error != NULL);
    ev.as.exec.error_message = r->error ? r->error->message : NULL;
    ev.as.exec.reason = (r->reason == BC_LLM_FINISH_REASON_STOP)        ? EDC_REASON_STOP
                        : (r->reason == BC_LLM_FINISH_REASON_TOOL_CALL) ? EDC_REASON_TOOL_CALL
                                                                        : EDC_REASON_OTHER;
    ev.as.exec.content = r->content;
    ev.as.exec.msg = r->msg;
    ev.as.exec.tool_calls_raw = r; /* 不透明：今天 dispatch_tools 收到的就是 resp 本身 */
    ev.as.exec.prompt_tokens = (int)r->usage.prompt_tokens;
    ev.as.exec.completion_tokens = (int)r->usage.completion_tokens;
    ev.as.exec.total_tokens = (int)r->usage.total_tokens;
    ev.as.exec.cached_tokens = (int)r->usage.cached_tokens;

    /* 账本要的归一化视图；只认 FUNCTION（与今天登记时的过滤一致）。
     * 纯借用：id/name/args 的所有权仍在 tool_calls 里，dispatch 之后不再触碰。 */
    struct edc_tool_call *calls = NULL;
    if (r->tool_calls)
    {
        unsigned n = utarray_len(r->tool_calls);
        calls = n ? calloc(n, sizeof(*calls)) : NULL;
        int m = 0;
        for (unsigned i = 0; calls && i < n; i++)
        {
            struct edc_llm_chat_tool_call *tc = utarray_eltptr(r->tool_calls, i);
            if (tc && tc->type == BC_LLM_TOOL_CALL_TYPE_FUNCTION)
            {
                calls[m].id = tc->id;
                calls[m].name = tc->function.name;
                calls[m].args = tc->function.arguments;
                m++;
            }
        }
        ev.as.exec.calls = calls;
        ev.as.exec.n_calls = m;
    }

    edc_step_outcome out = edc_executor_advance(ex, task, &ev);

    /* 被接手则回写 NULL（"消掉即置 NULL" 的约定）；未被接手就保持原样，
     * 让 resp 自己释放 —— 与今天逐字一致。
     * 注意 **不碰 r->tool_calls**：今天也不碰。 */
    r->msg = ev.as.exec.msg;
    free(calls);
    return out;
}

edc_step_outcome edc_executor_advance_tool_resp(struct edc_executor *ex, struct edc_task *task, void *request_id,
                                                void *req)
{
    struct edc_toolbox_actor_req *tr = req;
    if (!tr)
        return EDC_STEP_BLOCKED;

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_TOOL_DONE;
    ev.request_id = request_id;
    ev.as.tool_done.call_id = tr->call_id;
    ev.as.tool_done.name = tr->name;
    ev.as.tool_done.result = tr->result;
    ev.as.tool_done.has_next = (tr->next != NULL);

    return edc_executor_advance(ex, task, &ev);
}

bool edc_executor_view_tool_emit(void *emit_msg, struct edc_tool_emit_view *out)
{
    struct edc_toolbox_emit_msg *emit = emit_msg;
    if (!emit || !out)
        return false;
    out->header = emit->header;
    out->content = emit->content;
    out->render_pref = emit->render_pref;
    return true;
}
