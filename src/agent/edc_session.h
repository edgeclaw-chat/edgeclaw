#ifndef __BC_SESSION_H__
#define __BC_SESSION_H__

#include "edc_error.h"
#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_config;
struct edc_turn;

struct edc_timestamp_node
{
    sds ts;
    struct edc_timestamp_node *prev, *next;
};

/* ── Forward declarations for task/group ───────────────────── */
struct edc_task;
struct edc_task_group;

/* ── Session ───────────────────────────────────────────────── */

struct edc_session
{
    char *session_id;
    char *name;      /* user-friendly display name */
    sds created_at;  /* ISO 8601 creation timestamp */
    sds last_active; /* ISO 8601 last-active timestamp */

    /* Phase 4: 新旧双轨 — groups/tasks (新) + messages (旧, Phase 5 移除) */
    struct edc_task_group *groups; /* 双向链表: 所有 process groups */
    struct edc_task *active_task;  /* foreground group 内的 active task */

    /* 该 session 自己的前台 group (与 group->session 一样是"属主"信息)。切换
     * session 时前台组随 session 保留, 后台 session 的落盘也能带回正确的
     * foreground_group_id —— 不再依赖 scheduler 只认一个前台组。 */
    struct edc_task_group *foreground_group;

    /* 内存里的 groups/tasks 是否已就绪。首次激活 (启动或切到未加载过的
     * session) 时从 session.json 反序列化一次; 之后内存即为权威, 再次切回来
     * 绝不能再 load_state —— 否则会用磁盘副本覆盖正在后台执行的活动对象。 */
    bool state_loaded;

    /* backward compat — Phase 5 移除 */
    cJSON *messages;
    struct edc_config *config;
    int save_skip_count;
    struct edc_timestamp_node *timestamps;

    sds history_dir; /* workspace/<session_id> — 按 session 存历史 */

    /* 绑定的工作目录 (绝对 realpath)。NULL = 未指定 → 回退到启动目录
     * (config->sandbox_root_real)。工具执行 (exec_shell 默认 cwd、fs 工具相对
     * 路径) 与 system prompt 的 {{cwd}} 都以它为准。 */
    sds workdir;

    uint32_t next_completion_seq; /* 完成顺序计数器 (从 1 开始) */
};

/* ── 生命周期 ──────────────────────────────────────────────── */

/** Create a session with a specific id and display name. */
extern struct edc_session *edc_session_new(struct edc_config *config, const char *session_id, const char *name);

/** Create a session bound to a working directory. |workdir| is resolved with
 *  realpath() and must name an existing directory; NULL/empty keeps the
 *  default (the launch directory). If |workdir| is given but unusable, creation
 *  FAILS (returns NULL) rather than silently binding the default. */
extern struct edc_session *edc_session_new_with_workdir(struct edc_config *config, const char *session_id,
                                                        const char *name, const char *workdir);

/** The session's bound working directory as an absolute realpath, or NULL when
 *  the session is unbound (callers then fall back to the launch directory,
 *  config->sandbox_root_real). */
extern const char *edc_session_get_workdir(struct edc_session *session);

/** Resolve |workdir| to the absolute realpath of an existing directory.
 *  An absolute path is used as-is; a leading "~"/"~/" expands to the home
 *  directory; any other relative path resolves against |base| (NULL/empty → the
 *  process cwd), so a caller can anchor it at a session's current working
 *  directory. Returns a new sds (caller frees), or NULL with *error (when
 *  non-NULL) set to the reason: empty path, nonexistent path, or not a
 *  directory. This is the single place the "relative → absolute + must exist"
 *  rule lives. */
extern sds edc_session_resolve_workdir(const char *base, const char *workdir, EdcError **error);

/** Bind the session to |workdir| (absolute or relative to the process cwd).
 *  Resolves it with realpath(), requires an existing directory, and grants it
 *  read+write in the sandbox (edc_config_add_sandbox_rw). Pass NULL/empty to
 *  unbind (revert to the launch directory). Returns true on success; on failure
 *  *error (when non-NULL) carries the reason and the binding is unchanged. */
extern bool edc_session_set_workdir(struct edc_session *session, const char *workdir, EdcError **error);
/** Rebuild session->messages from persisted history. NOTE: the session lifecycle
 *  no longer calls this (LLM context lives in group->history, not
 *  session->messages). It is a no-op rebuild while the session has live groups,
 *  whose history references the current messages — rebuilding would dangle. */
extern void edc_session_reload(struct edc_session *session, bool load_history);
extern void edc_session_free(struct edc_session *session);

/** Build the "/session status" report for |session|: group/task counts, the
 *  per-state task breakdown, and — when |is_current| — the foreground group
 *  (|foreground_group_id|, may be NULL) and the session's active task. Returns
 *  a new sds (caller frees). */
extern sds edc_session_status_text(struct edc_session *session, bool is_current, const char *foreground_group_id);

/** Reset session to empty state (clear messages, tasks, groups),
 *  keeping the same id/name. Does NOT save or load. */
extern void edc_session_reset(struct edc_session *session);

/** Get the display name (falls back to session_id). */
extern const char *edc_session_get_name(struct edc_session *session);

/* ── 消息 / 历史 (不变签名) ───────────────────────────────── */

extern void edc_session_add_turn(struct edc_session *sess, struct edc_turn *turn);
extern cJSON *edc_session_get_messages(struct edc_session *session);
extern size_t edc_session_get_message_count(struct edc_session *session);
extern bool edc_session_save_history(struct edc_session *session);

/* ── Phase 4 新增: Task/Group 管理 ──────────────────────────── */

extern void edc_session_add_group(struct edc_session *session, struct edc_task_group *group);
extern void edc_session_remove_group(struct edc_session *session, struct edc_task_group *group);
extern struct edc_task_group *edc_session_find_group(struct edc_session *session, const char *group_id);
extern struct edc_task *edc_session_find_task(struct edc_session *session, const char *task_id);

extern void edc_session_set_active_task(struct edc_session *session, struct edc_task *task);
extern struct edc_task *edc_session_get_active_task(struct edc_session *session);

/* ── Phase 4 新增: 消息切片 (fork 上下文) ──────────────────── */

extern cJSON *edc_session_get_messages_slice(struct edc_session *session, size_t from, size_t count);

/* ── Phase 4 新增: Task 结构持久化 ─────────────────────────── */

extern bool edc_session_save_tasks(struct edc_session *session);
extern void edc_session_reload_tasks(struct edc_session *session);

/* ── Phase 2: 完整状态持久化 ───────────────────────────────── */

struct edc_scheduler;

/** Save complete session state to workspace/<session_id>/session.json.
 *  Atomic: writes to .tmp then rename().
 *  Prerequisite: edc_session_save_history() should be called first.
 *  Returns true on success. */
extern bool edc_session_save_state(struct edc_session *session, struct edc_scheduler *sched);

/** Load session state from workspace/<session_id>/session.json.
 *  Rebuilds all groups, tasks, turns, and dependencies.
 *  Prerequisite: session->messages must already be loaded.
 *  Returns true on success, false if file missing or corrupt. */
extern bool edc_session_load_state(struct edc_session *session, struct edc_scheduler *sched);

#endif
