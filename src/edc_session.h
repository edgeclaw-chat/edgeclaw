#ifndef __BC_SESSION_H__
#define __BC_SESSION_H__

#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_config;
struct edc_turn;

struct edc_timestamp_node {
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
    char *name;               /* user-friendly display name */
    sds created_at;           /* ISO 8601 creation timestamp */
    sds last_active;          /* ISO 8601 last-active timestamp */

    /* Phase 4: 新旧双轨 — groups/tasks (新) + messages (旧, Phase 5 移除) */
    struct edc_task_group *groups;      /* 双向链表: 所有 process groups */
    struct edc_task *active_task;       /* foreground group 内的 active task */

    /* backward compat — Phase 5 移除 */
    cJSON *messages;
    struct edc_config *config;
    int save_skip_count;
    struct edc_timestamp_node *timestamps;

    sds history_dir;    /* workspace/<session_id> — 按 session 存历史 */

    uint32_t next_job_id;   /* 下一个 job ID (从 1 开始递增) */
    uint32_t next_completion_seq;  /* 完成顺序计数器 (从 1 开始) */
};

/* ── 生命周期 ──────────────────────────────────────────────── */

/** Create a session with a specific id and display name. */
extern struct edc_session *edc_session_new(struct edc_config *config,
                                            const char *session_id,
                                            const char *name);
extern void edc_session_reload(struct edc_session *session, bool load_history);
extern void edc_session_free(struct edc_session *session);

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

#endif
