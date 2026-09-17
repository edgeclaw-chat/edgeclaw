#ifndef __EDC_SESSION_STORE_H__
#define __EDC_SESSION_STORE_H__

#include <stdbool.h>

/* ── session store 的读口与生命周期 ────────────────────────────
 *
 * 这些操作每一个都同时需要两样东西: **store 自己** (`session_manager`: session
 * 与 task 的家、active_index 的唯一权威) 和 **task 编排** (`scheduler`: task 图、
 * 信号、落盘请求)。所以它们显式收这两者 —— 而不是借某个伞对象捎带
 * (P4 形状 C: 签名即读者名册)。
 *
 * 这里**不放**发布/上屏 (那是 channel 的词汇), 也不放 CLI history (那是前端的)。 */

struct edc_scheduler;
struct edc_session;
struct edc_session_manager;
struct edc_task;

/** Resolve a task id across sessions, activating its owning session on demand.
 *
 *  Task ids are globally unique (see edc_session_manager_find_task_any), so a
 *  hit is unambiguous. Unlike the plain in-memory lookup, this also handles the
 *  case where the owning session exists on disk but has not been activated this
 *  run: it probes the unloaded sessions' session.json, and on a match loads that
 *  session (lazy activation) before retrying.
 *
 *  Returns NULL when the id is unknown on disk either (callers then fail the
 *  command — no fallback). |owner_out| (optional) receives the owning session,
 *  or NULL on miss. Loading is side-effect-free with respect to the active
 *  session: the foreground group lives on each session, not on the scheduler. */
extern struct edc_task *edc_session_store_resolve_task(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                                       const char *task_id, struct edc_session **owner_out);

/** Load a session's persisted state and reconcile any cross-session dependency
 *  edges it revealed. Marks the session loaded (memory is authoritative even for
 *  a brand-new session with no state file). This is the single entry point for
 *  activating a session; call it from every load site. */
extern bool edc_session_store_load_session(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                           struct edc_session *session);

/** Resolve and wire pending cross-session dependency edges (recorded during
 *  deserialize when a dep's source was not in the loaded session). Each pass may
 *  lazy-load another session, revealing more pending edges; the loop runs to a
 *  fixpoint. Entries that stay unresolved hold their target task (resume gate).
 *  Safe to call repeatedly; re-entrant calls are ignored (see sched->reconciling). */
extern void edc_session_store_reconcile_pending_deps(struct edc_session_manager *sm, struct edc_scheduler *sched);

/** session 标签 → session 对象 (P3 的 store 读口; 也是 shell/命令层解析"这条输入
 *  属于哪个 session"的入口)。
 *
 *  - 标签非空: 按 id/name 在 session_manager 里找, **找不到返回 NULL** (调用方拒绝
 *    这条输入, 不许回退到活跃 session —— 那正是老实现"切错台"的来源);
 *  - 标签为空/NULL: 返回活跃 session (没有 session 概念的入站, 如 crontab)。 */
extern struct edc_session *edc_session_store_resolve_session(struct edc_session_manager *sm, const char *session_id);

/** 退出落盘: 全量 history + **每个已加载** session 的 state (含后台 session)。
 *  本 run 从未激活过的 session 会被跳过 (空壳写盘会覆盖磁盘上更新的状态)。 */
extern void edc_session_store_save_all_sessions_state(struct edc_session_manager *sm, struct edc_scheduler *sched);

/** 单个 session 的落盘请求 (NULL = 活跃的那个): 先写 sessions 索引 (全量 history),
 *  再重写目标 session 的 state。
 *
 *  |session| 是**显式参数**: 每个调用点自己知道要存谁 (任务收尾时存属主 —— 后台
 *  session 的任务完成也要落盘, 哪怕前台显示的是别人; 显式取消时存被取消的那个) ——
 *  P3 ③ 起没有任何"当前 session"可以默认, NULL 只是"调用点确实没指定"时才用。
 *
 *  调用点都在 kernel 内 (scheduler 任务收尾 / 本文件的取消路径) —— 它原来是装配根
 *  注入的 `save_session` 端口, store 搬进 kernel 后端口成了同包中转, 于 2026-09-16
 *  退休 (见 runtime-model.md §11)。 */
extern void edc_session_store_save_session_state(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                                struct edc_session *session);

/** 取消该 session (NULL = 活跃) 的全部非终结 turns 并立即落盘, 使 CANCELLED
 *  跨重启保留 (`/session stop`)。 */
extern void edc_session_store_stop_all_turns(struct edc_session_manager *sm, struct edc_scheduler *sched);

/** 停止该 session 的全部 turn 但不落盘 (退出路径: 状态由退出落盘负责)。 */
extern void edc_session_store_remove_all_turns(struct edc_session_manager *sm, struct edc_scheduler *sched);

#endif
