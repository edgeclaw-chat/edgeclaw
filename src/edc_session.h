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

    /* Phase 4: 新旧双轨 — groups/tasks (新) + messages (旧, Phase 5 移除) */
    struct edc_task_group *groups;      /* 双向链表: 所有 process groups */
    struct edc_task *active_task;       /* foreground group 内的 active task */

    /* backward compat — Phase 5 移除 */
    cJSON *messages;
    struct edc_config *config;
    int save_skip_count;
    struct edc_timestamp_node *timestamps;
};

/* ── 生命周期 (不变) ───────────────────────────────────────── */

extern struct edc_session *edc_session_new(struct edc_config *config);
extern void edc_session_reload(struct edc_session *session, bool load_history);
extern void edc_session_free(struct edc_session *session);

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
