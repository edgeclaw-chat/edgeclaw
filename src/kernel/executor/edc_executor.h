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
#include <stdint.h>

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

/* 信号也是事件（docs/runtime-model.md §2.8）：迟到完成事件必须无害、信号直接改
 * task 状态。
 *
 * 以前这里还有个 `persist`（"显式取消要立即落盘"）—— 现在它是**宿主的局部事实**:
 * 发信号的那条路径只有 `/task stop`, 落盘由宿主在事件返回之后自己做（§2.2:
 * 持久化是容器的语义, 不属于 task 之内的推进）。executor 不再读它。 */
struct edc_event_signal
{
    edc_task_signal sig;
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

/* ── 一次推进的**事实** ──────────────────────────────────────
 * 这不是「效果清单」—— 里面没有命令、没有「请帮我做 X」。全是**已经在 task 上
 * 发生的事**的摘要, 由调用方（scheduler）自己决定据此告诉谁。
 *
 * 判据（docs/executor-outbound-boundary.md §2）:
 *   「发起下一步 I/O」是 executor 的事 —— 否则它没有推进;
 *   「把结果告诉外界」是 scheduler 的事 —— 否则它在管业务。 */
struct edc_advance_result
{
    edc_step_outcome outcome; /* DONE / BLOCKED / ERROR（语义不变） */

    /* 这次推进改了 step 或 life。executor 在入口处快照、出口处比较得出来 ——
     * 所以它覆盖了所有内部路径, 包括不经 edc_task_move 的信号路径。 */
    bool changed;

    /* 这次推进**决定**让任务终结（COMPLETED / ERROR / CANCELLED）。
     * 由分支**显式**给出, 不由 `changed` 推出来: 转移可能被状态机拒绝, 而
     * "已经决定终结"这件事仍然成立（消息照样要发出去）—— 与今天一致。 */
    bool settled;

    /* 要交付给外界的文本。**借用指针**, 有效期 = 「本函数返回后, 调用方处理完
     * 这个结果为止」（投递就发生在那一瞬间, 所以不需要所有权转移协议）。
     * 无产出则 NULL。executor 保证它活得住的办法见 .c 里的三条来源注释。 */
    const char *notice;
};

/* ── 出站 ops（kernel → 世界）──────────────────────────────── */

/* `ctx` 一律是 scheduler（executor 不持有它，只透传）。
 *
 * **准入判据 = "发起下一步 I/O"**（docs/executor-outbound-boundary.md §2）:
 *
 *   > 发起下一步 I/O 是 executor 的事 —— 否则它没有推进;
 *   > 把结果告诉外界是 scheduler 的事 —— 否则它在管业务。
 *   > 状态机是 task 模块的事。
 *
 * 这是**职责**判据, 不是位置判据。前两版判据都不够用, 记在这里避免第三次返工:
 *   - "需不需要宿主私有状态" → 把 task 之间的依赖边与内容轴误删 (薄 thunk ≠ 之内);
 *   - "是不是 task 之内"     → 漏掉 `send_reply` (它既非之内也非之间, 是"出 kernel",
 *                              被包依赖环这个理由掩盖过去了)。
 * `send_llm` 与 `send_reply` 住在**同一个包依赖环**里, 位置判据分不开它们;
 * 职责判据一句话分开: **`send_reply` 不发起, 任务照样推进完了。**
 *
 * 所以这张表**只剩 2 项**, 而且它们也正好是那个环的两条边 —— 两个独立理由
 * 指向同一结果:
 *
 *   - `send_llm`        —— 推理, 推进的下一步;
 *   - `dispatch_tools`  —— 工具 I/O, 推进的下一步。
 *
 * **其余 9 项不是"换个地方调用", 而是从接口里消失** —— 它们变成
 * `struct edc_advance_result`（事实）+ scheduler 的 `sched_apply_advance`（策略）:
 *
 *   | 消失的 op | 现在归谁 |
 *   |---|---|
 *   | set_task_state / unregister_task | executor 直调 `edc_task_move`; 簿记在 scheduler |
 *   | mark_dirty | scheduler（推 TUI 是展示逻辑） |
 *   | notify_dependents / start_queued_dependents | scheduler（依赖 DAG = task 之间） |
 *   | deliver_group_memory / send_reply | scheduler（产出**给谁** = 业务/装配） |
 *   | next_completion_seq | scheduler（容器内的完成次序 = task 之间） |
 *   | on_task_settled | scheduler（收尾的两个订阅者：折叠 + 落盘） |
 *
 * **仍然不在这张表里的**（task 之内, 或只需读 config）:
 *   - task 模块调用（`edc_task_*` / `edc_turn_*`，含 `edc_task_flush_redirects`）；
 *   - `edc_task_move`（状态机；非法转移由它拒绝）；
 *   - `max_iter` → 直读 `config`（它是**运行预算**：决定要不要续发）。
 *
 * **可机械跑的判据**（runtime-model.md §8；口径同判据 5 = **剥掉注释后数标识符**）:
 *   ① 本头文件剥注释后，session / channel / fold / 脏表 四类符号计数全为 **0**;
 *   ② `edc_executor.c` 里 `ops->` 只剩 4 处（send_llm×2 + dispatch_tools×2 ——
 *      每个「×2」是一次空判 + 一次调用）。
 *   两条的完整命令记在 runtime-model.md §8。 */
struct edc_executor_ops
{
    /* 派发一轮 LLM 请求。调用方负责已自增 llm_rounds（与今天一致）。 */
    void (*send_llm)(void *ctx, struct edc_task *task, void *request_id);

    /* 派发本轮工具调用链。 */
    void (*dispatch_tools)(void *ctx, struct edc_task *task, void *tool_calls, void *request_id);
};

/* ── 边界适配 ────────────────────────────────────────────────
 * actor 消息的负载类型（edc_llm_actor_resp / edc_toolbox_actor_req /
 * edc_toolbox_emit_msg）只在这里被认识 —— 这样 scheduler 不再需要
 * include edc_llm_actor.h / edc_toolbox_actor.h（§8 验收判据 3）。
 *
 * 负载一律以 `void *` 传入：适配是**类型无关的调用约定**，调用方
 *（scheduler 的分发器）连结构体名字都不必写。 */

/* BC_ACTOR_MSG_LLM_RESPONSE */
struct edc_advance_result edc_executor_advance_llm_resp(struct edc_executor *ex, struct edc_task *task,
                                                       void *request_id, void *resp);

/* BC_ACTOR_MSG_TOOL_RESPONSE */
struct edc_advance_result edc_executor_advance_tool_resp(struct edc_executor *ex, struct edc_task *task,
                                                         void *request_id, void *req);

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
    /* 进程级配置 (只读)。executor 需要的**环境事实**（api_key 有无 / max_iter）
     * 直接从这里读 —— 它们是配置只读, 不是宿主私有状态, 没必要走端口。
     * 只有指针, 所有权仍在 app；scheduler 构造时把 config 递进来。 */
    const struct edc_config *config;
};

void edc_executor_init(struct edc_executor *ex, const struct edc_executor_ops *ops, void *ctx,
                       const struct edc_config *config);

/* 拿到一个事件，决定下一步并触发副作用。
 * 调用前提：task 已由 scheduler 按 request_id 找到，且未被取消
 *（`task->cancelled_request` 的丢弃逻辑留在 scheduler —— 它是 task 之间的
 * 取消状态，不是 task 之内的推进）。
 * **ev 非 const**：其中 `msg` / `tool_calls_raw` 的所有权会按需**转移**，
 * 被接手后 advance 把它们置 NULL —— 与今天 sched_handle_llm_resp 的
 * "消掉即置 NULL" 约定逐字一致（适配器据此回写源结构，避免双重释放）。
 * 返回值见 `struct edc_advance_result`；调用方**必须**把它交给策略点
 * （scheduler 的 sched_apply_advance），否则簿记/交付会丢。 */
struct edc_advance_result edc_executor_advance(struct edc_executor *ex, struct edc_task *task, struct edc_event *ev);

#endif /* EDC_EXECUTOR_H */
