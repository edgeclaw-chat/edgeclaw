#ifndef __BC_AGENT_H__
#define __BC_AGENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_toolbox.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_task;
/* 命令处理器的签名说的是 **channel 消息** (userland 的词汇)。core (scheduler) 已经
 * 不认识这个类型了 —— 它只透传一个不透明指针 (input->origin), 由这里解释。 */
struct edc_channel_message;

struct edc_agent
{
    struct edc_config *config;
    struct edc_session_manager *session_manager;
    struct edc_toolbox *toolbox;
    struct edc_scheduler *scheduler;

    edc_actor_t *channel_actor;
    edc_actor_t *llm_actor;
    edc_actor_t *toolbox_actor;

    edc_actor_runner_t *runner;

    /* CLI history cache (for !n / !! expansion) */
    sds *history;
    int history_count;
    int history_capacity;

    /* Guards edc_agent_reconcile_pending_deps against re-entry: resolving a dep
     * may lazy-load another session, whose own deps must be drained by the
     * outer loop rather than recursively. */
    bool reconciling;
};

extern void edc_agent_add_history(struct edc_agent *agent, const char *line);

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
extern struct edc_task *edc_agent_resolve_task(struct edc_agent *agent, const char *task_id,
                                               struct edc_session **owner_out);

/** Load a session's persisted state and reconcile any cross-session dependency
 *  edges it revealed. Marks the session loaded (memory is authoritative even for
 *  a brand-new session with no state file). This is the single entry point for
 *  activating a session; call it from every load site. */
extern bool edc_agent_load_session(struct edc_agent *agent, struct edc_session *session);

/** Resolve and wire pending cross-session dependency edges (recorded during
 *  deserialize when a dep's source was not in the loaded session). Each pass may
 *  lazy-load another session, revealing more pending edges; the loop runs to a
 *  fixpoint. Entries that stay unresolved hold their target task (resume gate).
 *  Safe to call repeatedly; re-entrant calls are ignored. */
extern void edc_agent_reconcile_pending_deps(struct edc_agent *agent);

extern char *edc_agent_run_turn(struct edc_agent *agent, const char *input);

/** session 标签 → session 对象 (P3 的 store 读口; 也是 shell/命令层解析"这条输入
 *  属于哪个 session"的入口)。
 *
 *  - 标签非空: 按 id/name 在 session_manager 里找, **找不到返回 NULL** (调用方拒绝
 *    这条输入, 不许回退到活跃 session —— 那正是老实现"切错台"的来源);
 *  - 标签为空/NULL: 返回活跃 session (没有 session 概念的入站, 如 crontab)。 */
extern struct edc_session *edc_agent_resolve_session(struct edc_agent *agent, const char *session_id);

/** 通知前端"活跃 session 变成 X 了" (P3 ①b): 用户 `/session switch|new` 成功后由
 *  命令层调用。前端据此换显示的那棵树并去拉它 —— 快照本身仍然由前端点名拉取。 */
extern void edc_agent_notify_active_session(struct edc_agent *agent, const char *session_id);
/* |sandbox_override|: -1 = keep the persisted config.json value (the default
 * at startup), 0 = force off, 1 = force on. The override is per-run and is NOT
 * persisted; only the in-TUI /sandbox command writes config.json. */
extern struct edc_agent *edc_agent_new(bool setup, int sandbox_override);
extern void edc_agent_remove_all_turns(struct edc_agent *agent);
extern void edc_agent_stop_all_turns(struct edc_agent *agent);
extern void edc_agent_free(struct edc_agent *agent);

/* Update the LLM provider settings at runtime (from the TUI /connect menu):
 * persist api_url/model/api_key to config.json, update the in-memory config,
 * and refresh the llm_actor's copy. A NULL/empty argument leaves that field
 * unchanged. Returns true when the config was persisted. */
extern bool edc_agent_set_llm_config(struct edc_agent *agent, const char *api_url, const char *model,
                                     const char *api_key);

/* Toggle the filesystem sandbox at runtime and persist the choice to
 * config.json (from the TUI /sandbox command). Returns true when persisted. */
extern bool edc_agent_set_sandbox_enabled(struct edc_agent *agent, bool enable);

/* Add (|add|=true) or remove (|add|=false) |path| from a sandbox path list,
 * used by /sandbox add-ro|rm-ro|add-rw|rm-rw|add-deny|rm-deny. |kind| selects
 * the ro / rw / deny list. |path| must be an absolute path to an existing
 * directory; it is canonicalized with realpath(). Persists to config.json and
 * updates the live config. Returns true on success; on failure writes a reason
 * into *msg when non-NULL (caller frees the sds). */
extern bool edc_agent_set_sandbox_path(struct edc_agent *agent, const char *path, edc_sandbox_path_kind kind, bool add,
                                       sds *msg);

/* 注册命令处理器 (由 shell 模块在初始化时调用) */
typedef bool (*edc_agent_command_fn)(struct edc_agent *, struct edc_actor *, struct edc_channel_message *);
extern void edc_agent_set_command_handler(edc_agent_command_fn fn);

#ifdef __cplusplus
}
#endif

#endif
