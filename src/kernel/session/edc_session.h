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

    /* 任务状态的唯一表示 (v3 起): groups/tasks。旧的 session->messages 轨道
     * 与 v2 的读取兼容都已移除, 这里不再是"新旧双轨"。 */
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

    /* 隔离标记: 本次 load_state 发现 session.json 存在却读不动 (版本不认识 /
     * JSON 坏), 已把它改名成 session.json.broken 时置位。置位后本进程**拒绝
     * 落盘** —— 否则内存里的空壳会被写进 session.json 这个槽, 于是 .broken 之
     * 外又多出一个"看起来有效"的假状态, 事后放进这个槽的恢复文件会被它在下
     * 一次退出时覆盖 (2026-09-15 二次覆盖事故: 隔离本身保住了字节, 却没保住
     * 槽位)。 */
    bool state_quarantined;

    struct edc_config *config;

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

/* ── 消息 / 历史 ───────────────────────────────────────────── */

extern size_t edc_session_get_message_count(struct edc_session *session);

/* ── Phase 4 新增: Task/Group 管理 ──────────────────────────── */

extern void edc_session_add_group(struct edc_session *session, struct edc_task_group *group);
extern void edc_session_remove_group(struct edc_session *session, struct edc_task_group *group);
extern struct edc_task_group *edc_session_find_group(struct edc_session *session, const char *group_id);

/** session 的**系统组** (kind == SYSTEM), 没有时返回 NULL。
 *
 *  系统任务 (fold 等) 的容器: 首次需要时由 edc_scheduler_spawn_system_task 建,
 *  最后一个系统任务被回收时由回收路径删掉 (runtime-model.md §4.6)。所以它可能
 *  在任何时刻不存在, 调用方必须处理 NULL。
 *
 *  这是 `kind` 在核心侧**唯一**的读取点, 而且是**结构性**的 ("这些任务归哪个
 *  容器"), 不隐藏、不跳过、不改变任何行为 —— 老 `is_system` 那种"用它当 skip
 *  的门"在这里没有对应物。另一个读者是展示层 (侧栏把这块画在单独一块)。 */
extern struct edc_task_group *edc_session_find_system_group(struct edc_session *session);

extern struct edc_task *edc_session_find_task(struct edc_session *session, const char *task_id);

extern void edc_session_set_active_task(struct edc_session *session, struct edc_task *task);
extern struct edc_task *edc_session_get_active_task(struct edc_session *session);

/* ── Phase 2: 完整状态持久化 ───────────────────────────────── */

struct edc_scheduler;

/** Save complete session state to workspace/<session_id>/session.json.
 *  Atomic: writes to .tmp then rename().
 *  The caller must only save a session whose state was loaded in this run
 *  (session->state_loaded): an unloaded session is just an id/name shell built
 *  from sessions.json, so writing it would clobber the newer on-disk state
 *  (groups, tasks, and the bound working directory). See
 *  edc_session_manager_save_loaded_states() for the exit path.
 *  Returns true on success. */
extern bool edc_session_save_state(struct edc_session *session, struct edc_scheduler *sched);

/** Load session state from workspace/<session_id>/session.json.
 *  Rebuilds all groups, tasks, turns, and dependencies.
 *  Returns true on success, false if file missing or corrupt. */
extern bool edc_session_load_state(struct edc_session *session, struct edc_scheduler *sched);

#endif
