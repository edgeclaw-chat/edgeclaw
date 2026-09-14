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
struct edc_channel_message;

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

/** 检查/处理命令 (返回 true=已处理) */
typedef bool (*edc_scheduler_command_fn)(struct edc_scheduler *sched, struct edc_actor *actor,
                                         struct edc_channel_message *msg);

/** 触发 memory compress */
typedef void (*edc_scheduler_memory_fn)(struct edc_scheduler *sched);

/** 保存 session 状态 (实现里 history 全量落盘)。
 *  |session| 指定要落 state 的 session; NULL = 当前绑定的 sched->session。
 *  后台 session 里的任务完成时要显式传它, 否则状态会写进前台 session。 */
typedef void (*edc_scheduler_save_fn)(struct edc_scheduler *sched, struct edc_session *session);

/** task 树快照已就绪，交由上层 (agent) 回传给 TUI 的 channel adapter */
typedef void (*edc_scheduler_snapshot_fn)(struct edc_scheduler *sched, sds serialized);

/* ── 待解析的跨 session 依赖 ───────────────────────────────── */

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
    struct edc_session *session; /* 当前绑定的 session; 前台组是 session 的属性 */
    struct edc_config *config;

    /* task 之内的推进器（无状态步进函数 + 出站 ops）。见 edc_executor.h。
     * scheduler 只做"归一化 + 转发"，不再需要认识 edc_llm_actor / edc_toolbox。 */
    struct edc_executor executor;

    struct edc_sched_task_entry *task_hash; /* uthash: request_id → edc_task */

    /* 未解析的跨 session 依赖边 (重启后对账用); owned by the scheduler.
     * Drained by edc_agent_reconcile_pending_deps(); entries that cannot be
     * resolved hold their target task (see the resume gate). */
    struct edc_pending_dep *pending_deps;

    edc_actor_t *self;

    /* 回调 (Phase 5 由 edc_agent 注入) */
    edc_scheduler_send_llm_fn send_llm;
    edc_scheduler_send_reply_fn send_reply;
    edc_scheduler_send_tool_fn send_tool_message;
    edc_scheduler_dispatch_tools_fn dispatch_tools;
    edc_scheduler_redispatch_tools_fn redispatch_tools;
    edc_scheduler_command_fn handle_command;
    edc_scheduler_memory_fn trigger_memory;
    edc_scheduler_save_fn save_session;
    edc_scheduler_snapshot_fn snapshot_ready;
    /* 快照待下发 (C3 合并推送): 状态变化只置脏, 由 actor handler 在每条消息
     * 处理完之后最多下发一次 —— 突发天然合并。 */
    bool snapshot_pending;

    int64_t next_task_id;

    void *ctx; /* 回调上下文 (edc_agent*) */
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

extern struct edc_scheduler *edc_scheduler_new(struct edc_session *session, struct edc_config *config);
extern void edc_scheduler_free(struct edc_scheduler *sched);

extern edc_actor_t *edc_scheduler_get_actor(struct edc_scheduler *sched);

/* ── Task 管理 ─────────────────────────────────────────────── */

extern char *edc_scheduler_gen_task_id(struct edc_scheduler *sched);
/** 把已存在的 task_id (十进制) 纳入发号器高水位并落盘: 反序列化存活任务时
 *  调用, 保证重启后新编号越过旧存档里的最大编号。非纯十进制 id 忽略。 */
extern void edc_scheduler_observe_task_id(struct edc_scheduler *sched, const char *task_id);
extern struct edc_task *edc_scheduler_create_task(struct edc_scheduler *sched, const char *desc,
                                                  edc_channel_type channel_type);
extern struct edc_task_group *edc_scheduler_create_group(struct edc_scheduler *sched, struct edc_task *leader);
extern void edc_scheduler_add_group(struct edc_scheduler *sched, struct edc_task_group *group);

extern struct edc_task *edc_scheduler_find_task(struct edc_scheduler *sched, void *request_id);
extern struct edc_task *edc_scheduler_find_task_by_id(struct edc_scheduler *sched, const char *task_id);

/* ── 前台/后台 ─────────────────────────────────────────────── */

/** Foreground group is a **session** property (see edc_session.foreground_group);
 *  the scheduler holds no copy. set_foreground writes it to the group's owning
 *  session (or clears the bound session's when |group| is NULL). get_foreground
 *  reads the currently bound session's value (NULL when unbound). */
extern void edc_scheduler_set_foreground(struct edc_scheduler *sched, struct edc_task_group *group);
extern struct edc_task_group *edc_scheduler_get_foreground(struct edc_scheduler *sched);

/* ── 当前 session ───────────────────────────────────────────── */

/** 获取调度器当前绑定的 session (task 树/前台组都挂在它上面)。 */
extern struct edc_session *edc_scheduler_get_session(struct edc_scheduler *sched);

/** 切换调度器绑定的 session。切换后前台组清空 (旧 session 的前台概念失效),
 *  由命令层随后按需 reload/恢复。外部代码 (shell 命令、入口) 必须经此 API
 *  切换, 禁止直接写 scheduler->session。 */
extern void edc_scheduler_set_session(struct edc_scheduler *sched, struct edc_session *session);

/** 从 scheduler actor 取当前 session 绑定的工作目录 (绝对路径); 未绑定或
 *  session 缺失时返回 NULL。工具层据此决定 exec_shell 默认 cwd 与 fs 工具
 *  相对路径的解析基准。 */
extern const char *edc_scheduler_session_workdir(edc_actor_t *scheduler_actor);

/** 单次工具调用应使用的工作目录: 优先取 **发起该调用的 task 所属 session**
 *  绑定的 workdir, 这样后台 session 里的任务即使当前显示的是别的 session,
 *  也在自己的目录里执行。|task| 为 NULL (非 task 路径) 或 task 无 group 时
 *  回退到 scheduler 的当前 session —— 与 edc_scheduler_session_workdir 一致。 */
extern const char *edc_scheduler_task_workdir(edc_actor_t *scheduler_actor, struct edc_task *task);

/** 当前 session 中是否存在未终结 (非 COMPLETED/CANCELLED/ERROR/SUSPENDED)
 *  的任务。供入口层 (main.c 的 -c/脚本模式) 轮询等待使用, 无需遍历内部结构。 */
extern bool edc_scheduler_has_active_tasks(struct edc_scheduler *sched);

/* ── 注册/反注册 ──────────────────────────────────────────────── */

extern void edc_scheduler_register_task(struct edc_scheduler *sched, struct edc_task *task, void *request_id);
extern void edc_scheduler_unregister_task(struct edc_scheduler *sched, void *request_id);

/* ── 信号 ──────────────────────────────────────────────────── */

extern void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, const char *task_id, edc_task_signal sig);
/** Signal a specific task by pointer (already resolved, possibly belonging to a
 *  non-active session). Same semantics as the id-based variant; the id-based one
 *  delegates here after a session-scoped lookup. */
extern void edc_scheduler_send_signal_to_task_ptr(struct edc_scheduler *sched, struct edc_task *task,
                                                  edc_task_signal sig);
extern void edc_scheduler_send_signal_to_group(struct edc_scheduler *sched, const char *group_id, edc_task_signal sig);
/** 对**指定** session 下所有 group 发信号 (不依赖 sched->session)。
 *  用于"明确要收尾"的路径: 进程退出用 running_only=true 只掐在途执行;
 *  /session remove 用 running_only=false 全取消 (用户明确要移除)。
 *  切换 session 不再取消任何任务 —— 被切走的 session 继续在后台执行
 *  (见 edc_commands_handle_session_switch)。
 *  running_only=true 只命中 edc_task_is_running() 的任务 (LLM_PROCESSING /
 *  TOOL_CALLING), 其余状态原样保留; false 则清掉所有非终态任务。 */
extern void edc_scheduler_send_signal_to_session(struct edc_scheduler *sched, struct edc_session *session,
                                                 edc_task_signal sig, bool running_only);
extern void edc_scheduler_send_signal_to_all(struct edc_scheduler *sched, edc_task_signal sig);

/* ── resume ────────────────────────────────────────────────── */

/** 用户 resume 一个 SUSPENDED 任务: 按账本重新判定并驱动 ——
 *    1. 依赖未满足 → WAITING_DEPS;
 *    2. 账本有未完成工具 → EXECUTING 补一条"中断"响应 / PENDING 重新派发;
 *    3. 有待跑的 active_turn → LLM_PROCESSING 重发 LLM;
 *    4. 否则 → WAITING_INPUT。
 *  调用方需保证 task 属于本 sched 且当前状态为 SUSPENDED。 */
extern void edc_scheduler_resume_task(struct edc_scheduler *sched, struct edc_task *task);

#endif
