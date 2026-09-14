#ifndef __BC_SCHEDULER_H__
#define __BC_SCHEDULER_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_executor.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "sds.h"
#include <stdbool.h>

struct edc_session;
struct edc_config;

/* ── 入站输入: core 自己的词表 ────────────────────────────────
 *
 * 规则 8 说"向内是**事件**" —— 事件的**词表也归 core**: 投递方 (channel 边界 / 工具)
 * 把 channel 消息翻译成下面这个结构之后才投 `BC_ACTOR_MSG_CHANNEL_RECV`, 于是
 * scheduler **不认识 `struct edc_channel_message`**, 也不 include
 * `edc_channel_actor.h` —— core 对外部世界的形状一无所知。
 *
 * 翻译点就在边界上 (`edc_channel_input_new()` / `edc_channel_input_free()`,
 * 见 edc_channel_actor.h): 那里既认识 channel 消息、也认识这个结构,
 * 依赖方向是"适配层 → core", 不是反过来。 */
struct edc_sched_input
{
    const char *session_id; /* 属主标签; NULL/空 = 交给 store 读口给默认 (活跃 session) */
    const char *content;
    edc_channel_type type; /* 渠道类别 —— 今天只有 adapter (TUI); task 从这条输入继承它 */
    void *origin;          /* 原始 channel 消息: **只透传**给命令处理器 (用户态解释),
                            * scheduler 自己不碰 */
};

struct edc_sched_task_entry;
/* edc_sched_task_entry is an internal hash entry used by uthash.
   Declared here for the hash head pointer type only. */

/* ── 回调类型 ──────────────────────────────────────────────── */

struct edc_scheduler;

/** 发送 LLM 请求: task 上下文已就绪，回调构造并发送给 llm_actor */
typedef void (*edc_scheduler_send_llm_fn)(struct edc_scheduler *sched, struct edc_task *task, void *request_id);

/** 发送 channel 回复 */
typedef void (*edc_scheduler_send_reply_fn)(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                                            edc_channel_message_type message_type, void *request_id);

/** 发送 TOOL 类型 channel 消息 (带 header)；仅由 active task 触发。
 *  render_pref 由发送方(tool)声明，渲染端据此决定是否按 Markdown 渲染。 */
typedef void (*edc_scheduler_send_tool_fn)(struct edc_scheduler *sched, struct edc_task *task, const char *header,
                                           const char *content, edc_render_pref_t render_pref, void *request_id);

/** 分发 tool calls */
typedef void (*edc_scheduler_dispatch_tools_fn)(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                                void *request_id);

/** resume 时重发未完成的 tool 调用 (由 agent 注入): 把 task 账本里 PENDING 的记录
 *  重建 toolbox 请求链并投递 (首条 PENDING 由调用方提升 EXECUTING)。
 *  返回投递的条数 (0 = 无)。 */
typedef int (*edc_scheduler_redispatch_tools_fn)(struct edc_scheduler *sched, struct edc_task *task, void *request_id);

/** 检查/处理命令 (返回 true=已处理)。
 *  参数是 **core 的入站输入** (上面的 `struct edc_sched_input`); 命令处理器在用户态,
 *  需要原始 channel 消息时自己从 `input->origin` 取回 (不透明透传)。 */
typedef bool (*edc_scheduler_command_fn)(struct edc_scheduler *sched, struct edc_actor *actor,
                                         const struct edc_sched_input *input);

/** 触发 memory compress */
typedef void (*edc_scheduler_memory_fn)(struct edc_scheduler *sched);

/** 保存 session 状态 (实现里 history 全量落盘)。|session| 是**显式参数**:
 *  每个调用点自己知道要存谁 (任务完成时存属主, 退出时存所有已加载的) ——
 *  P3 ③ 起没有任何"当前 session"可以默认。 */
typedef void (*edc_scheduler_save_fn)(struct edc_scheduler *sched, struct edc_session *session);

/** session store 的读口: 把 `session_id` 解析成 session (出站调用 —— 规则 8 允许的
 *  store 读口)。
 *
 *  规则 3: scheduler **不知道"哪个是当前的"** (那是 session_manager.active_index 的
 *  事) —— 它只把 `session_id` 当**不透明作用域标签**, 需要对象时经这个口解析。
 *  `NULL`/空标签 = "调用方没指定", 由实现决定默认值 (agent 给活跃 session), 于是
 *  没有 session 概念的入站 (crontab 定时任务) 行为不变, 而 TUI 永远显式带 id。
 *  解析不出来 (标签指向不存在的 session) 返回 NULL —— 调用方**不许猜**、不许回退
 *  到"当前" (那正是 P3 要拆掉的那份副本)。 */
typedef struct edc_session *(*edc_scheduler_resolve_session_fn)(void *ctx, const char *session_id);

/** 前端**点名要的那棵树**（拉取的应答；①b 之后没有"主动把树推给你"的路径）。
 *
 *  实现把它作为 `EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE` 回转给前端；前端按
 *  `session_id` 认领自己的请求 —— 迟到（期间换过 session）的应答**丢弃**，
 *  这是任何 request/reply 都该有的正确性检查，不是替广播擦屁股。 */
typedef void (*edc_scheduler_snapshot_fn)(struct edc_scheduler *sched, sds serialized);

/** 某个 session 的 task 树**变了**（事件，**不带树**）。
 *
 *  P3 ①b：把"变了"与"是什么"分开 ——
 *    - **变了**（本回调，很小，自带 `session_id`）: 内核 ──► 前端 "session X 的树变了"；
 *    - **是什么**（`BC_ACTOR_MSG_TASK_SNAPSHOT`，显式带 `session_id`）: 前端 ──► 内核
 *      "给我 X 的树" ──► 前端。
 *
 *  于是**别人的树根本不序列化、不上路、不到达**：前端只拉自己正在显示的那一棵
 *  （权威在 TUI 的 `current_session_id_`，内核不需要知道谁在看什么）。
 *  `reason` 只用于 trace/日志（"谁把树弄脏的"），不上协议。
 *
 *  老做法是"任何变化都把**绑定 session** 的整棵树序列化后推进 channel" —— 那份
 *  推送同时承担了"变了"和"是什么"，于是它也就顺带决定了前端显示哪棵树
 *  （`SyncChatSession` 真的会换掉用户正在看的对话记录）。 */
typedef void (*edc_scheduler_snapshot_changed_fn)(struct edc_scheduler *sched, const char *session_id,
                                                  const char *reason);

/* ── 待解析的跨 session 依赖 + 待通知的脏 session ─────────── */

/** 一张"树变了"的表项: 哪个 session 变了 + 谁弄脏的 (只进 trace, 不上协议)。
 *  `reason` 是静态字面量, 不拥有。 */
struct edc_sched_dirty
{
    struct edc_session *session;
    const char *reason;
};

/** A cross-session dependency edge read back from disk whose source task was
 *  not in the loaded session's id_map. Reconcile resolves it later (possibly
 *  lazy-loading the owner session) and wires the edge. Stored **by id** so it
 *  can never dangle when a session/task is freed. */
struct edc_pending_dep
{
    sds target_task_id; /* the dependent task (in the session being loaded) */
    sds source_task_id; /* the missing dependency (may live in another session) */
    edc_task_dep_type type;
    bool completed;
    bool cancelled;
    bool injected;
    struct edc_pending_dep *next;
};

/* ── 调度器 ────────────────────────────────────────────────── */

struct edc_scheduler
{
    struct edc_config *config; /* (P3 ③: 不再持有"当前 session" —— 见 edc_scheduler.h 顶部的说明) */

    /* task 之内的推进器（无状态步进函数 + 出站 ops）。见 edc_executor.h。
     * scheduler 只做"归一化 + 转发"，不再需要认识 edc_llm_actor / edc_toolbox。 */
    struct edc_executor executor;

    struct edc_sched_task_entry *task_hash; /* uthash: request_id → edc_task */

    /* 未解析的跨 session 依赖边 (重启后对账用); owned by the scheduler.
     * Drained by edc_session_store_reconcile_pending_deps(); entries that cannot
     * be resolved hold their target task (see the resume gate).
     *
     * |reconciling| guards that drain against re-entry: resolving an entry may
     * lazy-load another session, and load_session reconciles by contract — the
     * inner call must let the outer fixpoint loop finish the table instead of
     * recursing. State **of this table's drain**, so it lives here, next to it. */
    struct edc_pending_dep *pending_deps;
    bool reconciling;

    edc_actor_t *self;

    /* 回调 (由 edc_runtime 的装配注入; 见 edc_runtime.c) */
    edc_scheduler_send_llm_fn send_llm;
    edc_scheduler_send_reply_fn send_reply;
    edc_scheduler_send_tool_fn send_tool_message;
    edc_scheduler_dispatch_tools_fn dispatch_tools;
    edc_scheduler_redispatch_tools_fn redispatch_tools;
    edc_scheduler_command_fn handle_command;
    edc_scheduler_memory_fn trigger_memory;
    edc_scheduler_save_fn save_session;
    edc_scheduler_snapshot_fn snapshot_reply;           /* 拉取的应答: 那棵树 */
    edc_scheduler_snapshot_changed_fn snapshot_changed; /* "某个 session 的树变了" (不带树) */
    edc_scheduler_resolve_session_fn resolve_session;   /* session_id → session (store 读口) */

    /* ── 哪些 session 的树变了 (待通知前端) ──
     * 一条消息里可能同时动了多个 session (跨 session 依赖唤醒目标), 也可能同一个
     * session 被标多次 —— 所以是一张**按 session 指针去重**的表, 而不是一个 bool。
     * 由 actor handler 在本条消息处理完之后一次性发完 (与回收同一位置、同一理由:
     * "本条消息引起的后果, 在消息末尾一次性落地")。 */
    struct edc_sched_dirty *dirty;
    size_t dirty_count;
    size_t dirty_cap;

    /* ── 待回收的 TRANSIENT 任务 (完成即回收) ──
     * 不在状态迁移的那一刻直接 free: 那条路径上还有调用方拿着 task 指针
     * (executor 的 exec_done 返回后, 调用它的响应处理还要接着用它)。所以只入队,
     * 由 actor handler 在**本条消息处理完之后**统一结算 —— 与 snapshot_pending
     * 同一个位置、同一个理由 ("本条消息引起的后果, 在消息末尾一次性落地")。
     * 数组按需增长; 队列在调度器销毁时只丢指针 (任务归 session 所有)。 */
    struct edc_task **reap_pending;
    size_t reap_count;
    size_t reap_cap;

    int64_t next_task_id;

    void *ctx; /* 回调上下文 (edc_runtime*) */
};

/* ── API ───────────────────────────────────────────────────── */

/* Record an unresolved cross-session dependency edge (its source was absent from
 * the loaded session's id_map). Copied by id. Drained by the agent reconcile. */
extern void edc_scheduler_add_pending_dep(struct edc_scheduler *sched, const char *target_task_id,
                                          const char *source_task_id, edc_task_dep_type type, bool completed,
                                          bool cancelled, bool injected);

/** True when |task| still has an unresolved cross-session dependency, i.e. it
 *  must not be resumed yet (the resume gate). */
extern bool edc_scheduler_task_has_unresolved_deps(struct edc_scheduler *sched, struct edc_task *task);

/** Source task id of the first unresolved cross-session dep for |task| (for a
 *  user-facing note), or NULL when there is none. */
extern const char *edc_scheduler_first_unresolved_dep(struct edc_scheduler *sched, struct edc_task *task);

/* P3 ③: 构造不再收 session —— scheduler **不持有**"当前 session"这份副本
 * (唯一权威是 session_manager.active_index)。每个入口显式带 session, 或经
 * store 读口 (resolve_session) 把标签解析成 session。 */
extern struct edc_scheduler *edc_scheduler_new(struct edc_config *config);
extern void edc_scheduler_free(struct edc_scheduler *sched);

extern edc_actor_t *edc_scheduler_get_actor(struct edc_scheduler *sched);

/* ── Task 管理 ─────────────────────────────────────────────── */

extern char *edc_scheduler_gen_task_id(struct edc_scheduler *sched);
/** 把已存在的 task_id (十进制) 纳入发号器高水位并落盘: 反序列化存活任务时
 *  调用, 保证重启后新编号越过旧存档里的最大编号。非纯十进制 id 忽略。 */
extern void edc_scheduler_observe_task_id(struct edc_scheduler *sched, const char *task_id);
extern struct edc_task *edc_scheduler_create_task(struct edc_scheduler *sched, struct edc_session *session,
                                                  const char *desc, edc_channel_type channel_type);

/** 创建一个 **system task** 并放进 `session` 的系统组 (没有就建一个)。
 *
 *  三个属性一次给全 (runtime-model.md §4.6 的四条规则):
 *    - `lifetime = TRANSIENT`  —— 不落盘、完成即回收 (回收后组空了会连组一起删);
 *    - `sink = GROUP_MEMORY`   —— 产出交给 group 记忆, 不发给用户;
 *    - 组 `kind = SYSTEM`      —— 只有展示层读它 (侧栏单独一块)。
 *
 *  `session` 是**属主**(也是系统组的家): 折叠一个后台 session 的组时, 任务与产出
 *  都落在那个 session 上 —— 这正是老实现做不到的 (它退回"保存当前 session")。
 *
 *  返回时任务停在 (READY, ALIVE): **它还没有 input** —— 调用方装上 turn (折叠
 *  prompt 等) 再派发 (见 edc_scheduler_dispatch_task)。任务归 session 所有,
 *  不要自己 free。没有 session 时返回 NULL。 */
extern struct edc_task *edc_scheduler_spawn_system_task(struct edc_scheduler *sched, struct edc_session *session,
                                                        const char *desc);

/** 该 session 的系统组里是否还有没跑完的 system task (即已有一次折叠在飞)。
 *  折叠触发用它做闸门: 一个 session 同时只折一个组, 免得两次折叠互相看见对方
 *  的半成品记忆。 */
extern bool edc_scheduler_has_pending_system_task(struct edc_session *session);

/** 给任务装上 input 并派发它: 建 turn → 注册 request_id → AWAIT_EXEC → send_llm。
 *  与 shell 的创建路径同一套步骤; system task (fold) 的触发方用它。 */
extern bool edc_scheduler_dispatch_task(struct edc_scheduler *sched, struct edc_task *task, const char *input);

/** system task 的 request_id 来源。
 *
 *  与 resume 同一套做法: 用一个**高位段**起步的计数器, 避免与 shell 输入路径的
 *  小整数 counter 撞 (task_hash 以指针为键, 撞了会把响应路由到别的任务)。
 *  段位: shell 输入 = 小整数, resume = 1<<40, system task = 1<<41。 */
extern void *edc_scheduler_gen_system_request_id(void);

/** 结算待回收的 TRANSIENT 任务 (完成即回收)。
 *
 *  actor handler 在**每条消息末尾**调用它 (先结算、再下发快照, 否则这一帧还会
 *  画出刚被回收的任务)。消息之外推进过任务状态的调用方 (单测、脚本路径) 也要
 *  自己调一次, 否则队列里的任务会留到下一轮。
 *  ⚠️ 它会 free 任务、必要时连空组一起删 —— 调用方此后不得再持有那些指针。 */
extern void edc_scheduler_drain_reap_pending(struct edc_scheduler *sched);
extern struct edc_task_group *edc_scheduler_create_group(struct edc_scheduler *sched, struct edc_session *session,
                                                         struct edc_task *leader);
/** 把 `group` 挂到 `session` 上 (显式 —— 组归 session 所有, P3 之后 scheduler 不再
 *  持有"当前 session")。`group` 自己记着属主 (`group->session`), 这里再用一次
 *  同一份信息只会制造第二处真相, 所以两个参数都要。 */
extern void edc_scheduler_add_group(struct edc_scheduler *sched, struct edc_session *session,
                                    struct edc_task_group *group);

extern struct edc_task *edc_scheduler_find_task(struct edc_scheduler *sched, void *request_id);
/** 在**指定** session 的组里按 id 找任务 (P3 ③: session 显式给 —— 以前是"在绑定
 *  的那个 session 里找")。跨 session 的查找走 store 的 `edc_session_store_resolve_task`
 *  (它用 session_manager 的全局 task-id 索引 + 惰性加载)。 */
extern struct edc_task *edc_scheduler_find_task_by_id(struct edc_scheduler *sched, struct edc_session *session,
                                                      const char *task_id);

/* ── 前台/后台 ─────────────────────────────────────────────── */

/** 前台组是 **session** 的属性 (edc_session.foreground_group)。
 *
 *  P3 ③ 起两个入口都**显式收 session**: `set_foreground(session, group)` 在
 *  group 非 NULL 时可用 group 的属主推出目标 (session 参数那时只是冗余);
 *  group 为 NULL (/task bg) 时用调用方给的 session —— 调用方 (命令层) 拿的是
 *  session_manager 的 active session, 不再问 scheduler 要。 */
extern void edc_scheduler_set_foreground(struct edc_session *session, struct edc_task_group *group);
extern struct edc_task_group *edc_scheduler_get_foreground(struct edc_session *session);

/** 单次工具调用应使用的工作目录: **发起该调用的 task 的属主 session** 绑定的
 *  workdir —— 这样后台 session 里的任务即使当前显示的是别的 session, 也在自己的
 *  目录里执行。任务没有属主 (无 group/session) 时返回 NULL, 由调用方决定兜底
 *  (工具层落到沙箱根): **scheduler 不再猜一个"当前 session"出来**。
 *  会话级 (无 task) 的调用方要目录请直接问 session_manager (TUI footer 走
 *  `edc_c_session_workdir`)。 */
extern const char *edc_scheduler_task_workdir(struct edc_task *task);

/** **指定** session 中是否存在未终结 (非 COMPLETED/CANCELLED/ERROR/SUSPENDED)
 *  的任务。供入口层 (脚本模式) 轮询等待使用, 无需遍历内部结构。
 *  P3 ③: session 由调用方给 (以前隐式用"绑定的那个")。 */
extern bool edc_scheduler_has_active_tasks(struct edc_scheduler *sched, struct edc_session *session);

/* ── 注册/反注册 ──────────────────────────────────────────────── */

extern void edc_scheduler_register_task(struct edc_scheduler *sched, struct edc_task *task, void *request_id);
extern void edc_scheduler_unregister_task(struct edc_scheduler *sched, void *request_id);

/* ── 信号 ──────────────────────────────────────────────────── */

extern void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, struct edc_session *session,
                                              const char *task_id, edc_task_signal sig);
/** Signal a specific task by pointer (already resolved, possibly belonging to a
 *  non-active session). Same semantics as the id-based variant; the id-based one
 *  delegates here after a session-scoped lookup. */
extern void edc_scheduler_send_signal_to_task_ptr(struct edc_scheduler *sched, struct edc_task *task,
                                                  edc_task_signal sig);
extern void edc_scheduler_send_signal_to_group(struct edc_scheduler *sched, struct edc_session *session,
                                               const char *group_id, edc_task_signal sig);
/** 对**指定** session 下所有 group 发信号。
 *  用于"明确要收尾"的路径: 进程退出用 running_only=true 只掐在途执行;
 *  /session remove 用 running_only=false 全取消 (用户明确要移除)。
 *  切换 session 不再取消任何任务 —— 被切走的 session 继续在后台执行
 *  (见 edc_commands_handle_session_switch)。
 *  running_only=true 只命中 edc_task_is_running() 的任务 (LLM_PROCESSING /
 *  TOOL_CALLING), 其余状态原样保留; false 则清掉所有非终态任务。 */
extern void edc_scheduler_send_signal_to_session(struct edc_scheduler *sched, struct edc_session *session,
                                                 edc_task_signal sig, bool running_only);
/** 对**指定** session 下所有 group 发信号 (P3 ③: session 由调用方给 ——
 *  "对所有"以前是指"对绑定的那个", 现在必须点名)。 */
extern void edc_scheduler_send_signal_to_all(struct edc_scheduler *sched, struct edc_session *session,
                                             edc_task_signal sig);

/* ── resume ────────────────────────────────────────────────── */

/** 用户 resume 一个 SUSPENDED 任务: 按账本重新判定并驱动 ——
 *    1. 依赖未满足 → WAITING_DEPS;
 *    2. 账本有未完成工具 → EXECUTING 补一条"中断"响应 / PENDING 重新派发;
 *    3. 有待跑的 active_turn → LLM_PROCESSING 重发 LLM;
 *    4. 否则 → WAITING_INPUT。
 *  调用方需保证 task 属于本 sched 且当前状态为 SUSPENDED。 */
extern void edc_scheduler_resume_task(struct edc_scheduler *sched, struct edc_task *task);

#endif
