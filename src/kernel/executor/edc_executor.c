/* edc_executor.c —— task 之内的推进器。见 edc_executor.h 与 docs/runtime-model.md §2。
 *
 * P1：本文件**原样搬来** sched_handle_llm_resp / sched_handle_tool_resp /
 * sched_handle_tool_started / sched_stop_at_iteration_cap 的**决策**部分，
 * 副作用改为经由 ops 出站。**行为不变**（搬迁纪律：逐行核对）。
 */
#include "edc_executor.h"
#include "edc_config.h"
#include "edc_exec_io.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "sds.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

void edc_executor_init(struct edc_executor *ex, const struct edc_executor_ops *ops, void *ctx,
                       const struct edc_config *config)
{
    ex->ops = ops;
    ex->ctx = ctx;
    ex->config = config;
}

/* ── 只读环境事实 (直读 config, 不走端口) ─────────────────────
 * 只剩 `max_iter` —— 它是**运行预算**（决定要不要继续续发下一轮）, 是"推进"的
 * 一部分。`no_api_key` 已移出: 它只被用来组装错误文案（展示），归宿主。 */
static int exec_max_iter(const struct edc_executor *ex)
{
    return (ex->config && ex->config->max_iter > 0) ? ex->config->max_iter : 0;
}

/* ── START：驱动一个任务 (唯一入口) ──────────────────────────
 *
 * "让任务开始跑"这件事只有这一份实现。调用方**不告诉它**该发推理还是该重派工具
 * —— 下一步由任务自己的样子回答, 顺序与从前 resume 的四段重判逐字一致:
 *
 *   1. 补齐悬空的 tool 响应 (取消/挂起打断在途工具留下的坑; 幂等);
 *   2. 账本里还有 PENDING 工具 → 重派它们, 等结果;
 *   3. 有待跑的 turn → 发起推理;
 *   4. 都没有 → 落回就绪。
 *
 * **task 之间**的那一段 (依赖未满足 → 挂回 AWAIT_DEPS) 不在这一格里: 依赖边是
 * scheduler 的地盘, 它在驱动之前当闸门判完 (见 edc_scheduler_drive_task)。
 *
 * 两个失败出口都**说原因**并让任务进 ERROR (而不是静默停住): 一个装不上端口的
 * 宿主不该留下一堆"看起来在跑"的僵尸, 而 ERROR 在 `/task resume` 接受的三类里,
 * 端口补上后一条 resume 就能续。 */
static struct edc_advance_result exec_io_failed(struct edc_task *task, const char *reason)
{
    edc_task_set_output(task, sdsnew(reason));
    edc_task_mark_failed(task);
    /* 进终态 = 不再欠任何请求 (与 exec_done 的两个终态出口同一条规矩): 留着那个
     * id 只会让"这条任务还在等谁"看起来成立。 */
    task->current_request_id = NULL;
    return (struct edc_advance_result){.outcome = EDC_STEP_ERROR, .settled = true, .notice = task->output};
}

static struct edc_advance_result exec_start(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const struct edc_executor_ops *ops = ex->ops;
    void *req_id = (void *)ev->request_id;

    /* 1. 悬空响应先补齐: assistant 消息带 tool_calls 而缺 tool 响应时, 下一次请求
     *    会被 API 拒绝。账本被取消清空过也算数 —— 消息才是权威。 */
    edc_task_repair_dangling_tool_calls(task);

    /* 2. 账本里还没执行的工具: 重派 (与当轮载荷那条 dispatch_tools 同一件事, 只是
     *    输入是账本)。 */
    if (edc_task_has_pending_tool_calls(task))
    {
        if (edc_task_move(task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE) && ops->redispatch_tools &&
            ops->redispatch_tools(ex->ctx, task, req_id) > 0)
            return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};

        /* 派不出去 = 这摊活跑不成 (没装工具箱端口 / 相位不允许这一步)。两条都进
         * ERROR 而不是"留在原地等下一次扫描": 一个推不动的任务不能继续当"可跑者"
         * —— 否则每轮扫描都会重试它 (而扫描是扫到不动点的)。 */
        return exec_io_failed(task, "[failed: the tool chain could not be re-dispatched (no toolbox port)]");
    }

    /* 3. 有待跑的 turn: 发起推理。 */
    if (task->active_turn)
    {
        /* 状态机拒绝 = 它此刻不在"能发起推理"的相位上。那句话说不出口就不发请求,
         * 免得留下一个没人认领的 request_id。 */
        if (!edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE))
            return exec_io_failed(task, "[failed: this task cannot be moved to AWAIT_EXEC (state machine)]");
        if (!ops->send_llm(ex->ctx, task, req_id))
            return exec_io_failed(task, "[failed: no LLM port is installed, so this task cannot be started]");
        task->llm_rounds++; /* 这一轮推理开始消耗预算 */
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
    }

    /* 4. 没有待跑内容 (resume 一个空任务): 落回就绪。 */
    edc_task_move(task, EDC_TASKSTEP_READY, EDC_LIFE_ALIVE);
    return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
}

/* ── EXEC_DONE：LLM 响应到达 ───────────────────────────────── */

static struct edc_advance_result exec_done(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
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
        task->current_request_id = NULL;
        /* 失败要广播给依赖者 (级联失败 / 抹边放行) —— 走 task 层的唯一入口, 不在这里直写。 */
        edc_task_mark_failed(task);
        if (e->content)
            edc_task_set_output(task, sdsnew(e->content));
        edc_task_flush_redirects(task);
        /* 交付交给宿主 —— 错误文案（含没配 API key 时的 /connect 指引）是**展示**,
         * 不是推进。notice 借用事件里的串, 调用期间必然有效。 */
        return (struct edc_advance_result){.outcome = EDC_STEP_ERROR,
                                           .settled = true,
                                           .notice = e->error_message ? e->error_message : "LLM request failed"};
    }

    if (e->reason == EDC_REASON_TOOL_CALL)
    {
        edc_task_move(task, EDC_TASKSTEP_AWAIT_TOOLS, EDC_LIFE_ALIVE);
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
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
    }

    if (e->reason == EDC_REASON_STOP)
    {
        task->current_request_id = NULL;

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
        edc_task_flush_redirects(task);
        edc_task_move(task, EDC_TASKSTEP_DONE, EDC_LIFE_COMPLETED);

        /* 到此**推进结束**。剩下的全是"告诉外界" —— 完成序号（容器内的完成次序）、
         * 置脏（推 TUI）、通知依赖者、启动排队依赖、产出交付（按 sink 选路）、
         * 折叠、落盘 —— 一律由宿主在 `sched_apply_advance` 里做。
         *
         * notice 借用 `e->content`（事件的属主是调用方, 本次调用期间有效）。
         * 注意 e->content 可能是 NULL —— 宿主按"有没有文本"处理, 与今天
         * send_reply(..., e->content, ...) 的行为一致。 */
        return (struct edc_advance_result){.outcome = EDC_STEP_DONE, .settled = true, .notice = e->content};
    }

    /* 未知 finish reason：与今天一致 —— ERROR + 系统消息（消息由宿主发）。 */
    task->current_request_id = NULL;
    /* 同上: 失败一律走 edc_task_mark_failed, 广播在它里面。 */
    edc_task_mark_failed(task);
    return (struct edc_advance_result){.outcome = EDC_STEP_ERROR,
                                       .settled = true,
                                       .notice = "Internal error: unknown finish reason"};
}

/* ── 到达 max_iter 上限 ──────────────────────────────────────
 * 构造一条 STOP 语义的事件 (内容为收尾说明), 复用 exec_done 的完整终结
 * 路径 —— turn 整体入库、任务置 COMPLETED、通知依赖者、触发收尾 (内容轴折叠
 * 与落盘), 保证被打断的上下文仍是连贯的。
 * （今天这步是伪造一个 edc_llm_actor_resp；归一化之后它就是一个事件。） */
static struct edc_advance_result stop_at_iteration_cap(struct edc_executor *ex, struct edc_task *task,
                                                       const char *request_id)
{
    int max_iter = exec_max_iter(ex);

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

    struct edc_advance_result r = edc_executor_advance(ex, task, &ev);

    /* 嵌套的 STOP 已经把通知 `sdsnew` 进 `task->output` —— 它活得比本函数长,
     * 所以 notice 指向它（本地这份可以安全释放）。这正是 §3.2 的三条来源之一。 */
    r.outcome = EDC_STEP_ERROR; /* 与今天一致：预算耗尽按 ERROR 返回 */
    r.notice = task->output;
    sdsfree(notice);
    return r;
}

/* ── TOOL_DONE：某个工具完成 ───────────────────────────────── */

static struct edc_advance_result tool_done(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
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
        int max_iter = exec_max_iter(ex);
        if (max_iter > 0 && task->llm_rounds >= max_iter)
        {
            return stop_at_iteration_cap(ex, task, ev->request_id); /* 已转入终结路径 */
        }

        edc_task_move(task, EDC_TASKSTEP_AWAIT_EXEC, EDC_LIFE_ALIVE);
        if (!ops->send_llm(ex->ctx, task, (void *)ev->request_id))
            return exec_io_failed(task, "[failed: no LLM port is installed, so the tool chain cannot continue]");
        task->llm_rounds++; /* 工具链结束后续发 → 再消耗一轮预算 */
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
    }
    return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
}

/* ── TOOL_STARTED：某个工具真正开始执行 ────────────────────── */

static struct edc_advance_result tool_started(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    const char *call_id = ev->as.call_id;
    if (call_id)
        edc_task_tool_call_mark_executing(task, call_id);
    return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
}

/* ── SIGNAL：取消/中断信号 ───────────────────────────────────
 * executor 只管**事件本身**：交给 task 层改状态（规则 1：不等任何回执）。
 * 反注册在途请求、要不要立刻落盘并推快照 —— 都是宿主的事，见
 * `edc_scheduler_send_signal_to_task_ptr`。 */
static struct edc_advance_result exec_signal(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    (void)ex;
    edc_task_send_signal(task, ev->as.signal.sig);

    bool cancelled = edc_task_is_cancelled(task);
    return (struct edc_advance_result){.outcome = cancelled ? EDC_STEP_DONE : EDC_STEP_BLOCKED,
                                       .settled = cancelled};
}

/* ── 步进 ──────────────────────────────────────────────────── */

struct edc_advance_result edc_executor_advance(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev)
{
    if (!ex || !task || !ev)
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};

    /* 相位快照：`changed` 由「入口 vs 出口」比较得出 —— 这样它自然覆盖**所有**
     * 内部路径，包括不经 `edc_task_move` 的信号路径（信号直接落到 task 层）。
     * 代价是两个枚举的比较；换来的是不必在六个出口手工记账（漏一处就是静默 bug）。 */
    edc_task_step step_before = task->step;
    edc_task_life life_before = task->life;

    struct edc_advance_result r;
    switch (ev->kind)
    {
    case EDC_EVENT_START:
        r = exec_start(ex, task, ev);
        break;
    case EDC_EVENT_EXEC_DONE:
        r = exec_done(ex, task, ev);
        break;
    case EDC_EVENT_TOOL_DONE:
        r = tool_done(ex, task, ev);
        break;
    case EDC_EVENT_TOOL_STARTED:
        r = tool_started(ex, task, ev);
        break;
    case EDC_EVENT_SIGNAL:
        r = exec_signal(ex, task, ev);
        break;
    default:
        r = (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};
        break;
    }

    r.changed = (task->step != step_before) || (task->life != life_before);
    return r;
}

/* ── 边界适配（本文件里唯一认识 actor 负载的地方）────────────
 * 负载以 void* 传入 —— 适配是**类型无关的调用约定**，scheduler 侧连结构体
 * 名字都不必出现（§8 验收判据 3）。 */

struct edc_advance_result edc_executor_advance_llm_resp(struct edc_executor *ex, struct edc_task *task, void *request_id,
                                                        void *resp)
{
    struct edc_llm_actor_resp *r = resp;
    if (!r)
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};

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

    struct edc_advance_result out = edc_executor_advance(ex, task, &ev);

    /* 被接手则回写 NULL（"消掉即置 NULL" 的约定）；未被接手就保持原样，
     * 让 resp 自己释放 —— 与今天逐字一致。
     * 注意 **不碰 r->tool_calls**：今天也不碰。 */
    r->msg = ev.as.exec.msg;
    free(calls);
    return out;
}

struct edc_advance_result edc_executor_advance_tool_resp(struct edc_executor *ex, struct edc_task *task, void *request_id,
                                                void *req)
{
    struct edc_toolbox_actor_req *tr = req;
    if (!tr)
        return (struct edc_advance_result){.outcome = EDC_STEP_BLOCKED};

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
