/* edc_executor.h —— task 之内的推进器（无状态步进函数）。
 *
 * 见 docs/runtime-model.md §2。分工：
 *   - **scheduler** 只管 **task 之间**：就绪集、依赖 DAG、并发槽、资源仲裁。
 *   - **executor** 只管 **task 之内**：拿到一个事件，决定下一步，并通过 ops 触发副作用。
 *
 * **无状态**：调用之间不保留任何东西。相位、工具账本、累积输出、预算全在 task 里
 * —— 这是 resume 强制的（必须活过崩溃的东西必须在 task 里）。
 *
 * **边界方向**（规则 8）：向外是**调用**（ops），向内是**事件**（edc_event）。
 * 本头文件**不认识** edc_llm_actor / edc_toolbox_actor：事件在边界处归一化。
 */
#ifndef EDC_EXECUTOR_H
#define EDC_EXECUTOR_H

#include "edc_config.h"
#include "edc_task.h"
#include "sds.h"
#include "cJSON.h"
#include <stdbool.h>

/* 前置声明：下面多处只用到指针。缺了它，原型里出现的 `struct edc_executor *`
 * 会在参数表作用域里自成一个类型，与后面的真正定义冲突（C 的经典坑）。 */
struct edc_executor;

/* ── 事件（向内）──────────────────────────────────────────── */

typedef enum
{
    EDC_EVENT_EXEC_DONE,    /* LLM 响应到达（含 error / tool_calls / stop） */
    EDC_EVENT_TOOL_STARTED, /* 某个工具真正开始执行 */
    EDC_EVENT_TOOL_DONE,    /* 某个工具完成 */
    EDC_EVENT_SIGNAL,       /* 取消/中断信号 */
} edc_event_kind;

/* LLM 响应的 **归一化** 形态。`reason` 用 executor 自己的枚举，
 * 不带 edc_llm_actor_finish_reason_t。
 * 所有权：本结构由调用者（scheduler）在栈上构造；`msg` 与 `tool_calls`
 * 的所有权在 advance 内**转移**（consumed），与今天 sched_handle_llm_resp
 * 的约定一致（消掉即置 NULL）。 */
typedef enum
{
    EDC_REASON_STOP = 0,   /* 正常收尾 */
    EDC_REASON_TOOL_CALL,  /* 要求调用工具 */
    EDC_REASON_OTHER,      /* length / sensitive / unknown → 今天的"未知"路径 */
} edc_reason;

/* 工具调用的**归一化**视图（账本登记用）。三个字段的所有权归调用者，
 * advance 期间有效。有了它，账本登记不必认识 edc_llm_chat_tool_call。 */
struct edc_tool_call
{
    const char *id;
    const char *name;
    cJSON *args; /* 账本 add 会深拷贝 */
};

struct edc_event_exec
{
    bool failed;              /* 网络/协议错误 */
    const char *error_message;/* failed 时非空 */
    edc_reason reason;
    sds content;              /* 最终文本（可 NULL） */
    cJSON *msg;               /* assistant 消息对象（所有权转入 turn） */
    /* 本轮 tool_calls：原样透传给 dispatch_tools 的**不透明负载**（今天传的
     * 就是整个 LLM 响应指针，所有权随 dispatch 转移）；同时给一份归一化视图
     * 供账本登记。 */
    void *tool_calls_raw;
    const struct edc_tool_call *calls;
    int n_calls;
    /* token 账：四条路径（error / tool_calls / stop / unknown）都要计入 */
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int cached_tokens;
};

struct edc_event_tool_done
{
    const char *call_id;
    const char *name;
    sds result;
    bool has_next; /* tool chain 里还有下一个 → 不续发 LLM */
};

/* 信号也是事件（docs/runtime-model.md §2.8）：迟到完成事件必须无害、
 * 信号直接改 task 状态、参数里的持久化只属于显式取消那条路。 */
struct edc_event_signal
{
    edc_task_signal sig;
    /* persist = 显式取消（用户 /task stop 那条路）要立即落盘 + 推快照。
     * group/session 的**扇出**今天就不落盘 —— 这个不对称原样保留（台账）。 */
    bool persist;
};

struct edc_event
{
    edc_event_kind kind;
    const char *request_id; /* 事件所属 task 的注册 id */
    union
    {
        struct edc_event_exec exec; /* EXEC_DONE */
        const char *call_id;        /* TOOL_STARTED */
        struct edc_event_tool_done tool_done;
        struct edc_event_signal signal;
    } as;
};

/* ── 决策结果（向外的一小部分；真正的动作走 ops）────────────── */

typedef enum
{
    EDC_STEP_BLOCKED, /* 已派发或在等：LLM 在跑 / 工具在跑 / 等依赖 */
    EDC_STEP_DONE,    /* 这一阶段结束（task 已 COMPLETED） */
    EDC_STEP_ERROR,   /* 失败（task 已 ERROR） */
} edc_step_outcome;

/* ── 出站 ops（kernel → 世界）──────────────────────────────── */

/* `ctx` 一律是 scheduler（executor 不持有它，只透传）。
 * 这些调用今天全部已在 sched_handle_* 里出现 —— 本接口只是把它们显式化，
 * **不新增任何行为**。 */
struct edc_executor_ops
{
    /* 派发一轮 LLM 请求。调用方负责已自增 llm_rounds（与今天一致）。 */
    void (*send_llm)(void *ctx, struct edc_task *task, void *request_id);

    /* 向 channel 发一条消息。 */
    void (*send_reply)(void *ctx, struct edc_task *task, const char *content, edc_channel_message_type msg_type,
                       void *request_id);

    /* ── sink = GROUP_MEMORY 的产出落地 ──
     * 产出送到哪由 `task->sink` 决定 (runtime-model.md §1.4 / §5 规则 7: 分流这一处
     * 在 executor, 镜像装配那处读点另算), 但"记忆"是什么、怎么合并、水位怎么推,
     * 是 scheduler 的事 —— 这里只把结果转交。实现见 sched_deliver_group_memory。
     * (fold 原来是 memory_actor 的专用协议 GROUP_FOLD_DONE; 现在它就是一次
     *  普通任务的完成, 产出按 sink 分流。) */
    void (*deliver_group_memory)(void *ctx, struct edc_task *task, const char *content);

    /* 把工具结果回显成一条 TOOL 消息（仅 active task 会触发）。 */
    void (*send_tool_message)(void *ctx, struct edc_task *task, const char *header, const char *content,
                              edc_render_pref_t render_pref, void *request_id);

    void (*dispatch_tools)(void *ctx, struct edc_task *task, void *tool_calls, void *request_id);

    /* ── task 之间的事：executor 只**请求**，由 scheduler 执行 ── */

    void (*set_task_state)(void *ctx, struct edc_task *task, edc_task_step step, edc_task_life life);
    void (*unregister_task)(void *ctx, void *request_id);
    void (*notify_dependents)(void *ctx, struct edc_task *task);
    void (*flush_redirects)(void *ctx, struct edc_task *task);
    /* 完成的任务其 session 的树可能变了 (turn 提交进 session, 侧栏计数变了)。
     * ①b 起这里是**事件**: 只通知属主 session "变了", 树要等前端点名才序列化 ——
     * 所以不再需要"是不是前台 active task"的闸门 (that gate compared against the
     * *bound* session)。 */
    void (*task_snapshot)(void *ctx, struct edc_task *task);
    /* 完成时补推快照的条件 —— "这个 task 此刻在被显示吗" = **展示逻辑**
     *（规则 8：active-task 闸门属于展示层，不属于 task 之内的推进）。 */
    /* task 完成后的依赖链再启动（task 之间的事，归 scheduler）。 */
    void (*start_queued_dependents)(void *ctx, struct edc_task *task);

    /* 前台状态变了 → 推一次通知（展示逻辑）。 */
    void (*notify_foreground_state)(void *ctx, struct edc_task *task);

    /* ── 环境查询 ── */
    struct edc_session *(*task_session)(void *ctx, struct edc_task *task); /* task 的属主 session */
    /* 今天 sched_handle_llm_resp 里的 `no_key` 表达式原样搬来（含 config 为
     * NULL 时判 false 的语义）—— 命名成 no_api_key 就是为了不含糊。 */
    bool (*no_api_key)(void *ctx);
    int (*max_iter)(void *ctx); /* <= 0 = 不限制 */

    /* ── 其它出站 ── */
    void (*trigger_memory)(void *ctx);
    void (*save_session)(void *ctx, struct edc_session *s);
};

/* ── 边界适配 ────────────────────────────────────────────────
 * actor 消息的负载类型（edc_llm_actor_resp / edc_toolbox_actor_req /
 * edc_toolbox_emit_msg）只在这里被认识 —— 这样 scheduler 不再需要
 * include edc_llm_actor.h / edc_toolbox_actor.h（§8 验收判据 3）。
 *
 * 负载一律以 `void *` 传入：适配是**类型无关的调用约定**，调用方
 *（scheduler 的分发器）连结构体名字都不必写。 */

/* BC_ACTOR_MSG_LLM_RESPONSE */
edc_step_outcome edc_executor_advance_llm_resp(struct edc_executor *ex, struct edc_task *task, void *request_id,
                                               void *resp);

/* BC_ACTOR_MSG_TOOL_RESPONSE */
edc_step_outcome edc_executor_advance_tool_resp(struct edc_executor *ex, struct edc_task *task, void *request_id,
                                                void *req);

/* TOOL_EMIT 的展示字段。emit 的负载被当成不透明指针，字段在这里取出 ——
 * 于是"该不该显示"的判断可以留在 scheduler（它是展示逻辑），
 * 而**结构体知识**留在这里。 */
struct edc_tool_emit_view
{
    const char *header;
    const char *content;
    edc_render_pref_t render_pref;
};
bool edc_executor_view_tool_emit(void *emit_msg, struct edc_tool_emit_view *out);

/* ── 推进 ──────────────────────────────────────────────────── */

struct edc_executor
{
    const struct edc_executor_ops *ops;
    void *ctx; /* 透传给 ops（= scheduler） */
};

void edc_executor_init(struct edc_executor *ex, const struct edc_executor_ops *ops, void *ctx);

/* 拿到一个事件，决定下一步并触发副作用。
 * 调用前提：task 已由 scheduler 按 request_id 找到，且未被取消
 *（`task->cancelled_request` 的丢弃逻辑留在 scheduler —— 它是 task 之间的
 * 取消状态，不是 task 之内的推进）。
 * **ev 非 const**：其中 `msg` / `tool_calls_raw` 的所有权会按需**转移**，
 * 被接手后 advance 把它们置 NULL —— 与今天 sched_handle_llm_resp 的
 * "消掉即置 NULL" 约定逐字一致（适配器据此回写源结构，避免双重释放）。 */
edc_step_outcome edc_executor_advance(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev);

#endif /* EDC_EXECUTOR_H */
