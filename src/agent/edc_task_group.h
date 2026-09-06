#ifndef __BC_TASK_GROUP_H__
#define __BC_TASK_GROUP_H__

#include "edc_task.h"
#include "sds.h"
#include <stdbool.h>

/* ── Task Group (Process Group, PGID) ──────────────────────────── */

struct edc_task_group
{
    char *group_id;           /* PGID (= leader 的 task_id) */
    struct edc_task *leader;  /* group leader task */
    struct edc_task *members; /* 链表头: group 内所有 task (含 leader) */
    int member_count;

    bool is_orphaned;            /* leader 退出后置 true */

    cJSON *history; /* 组内消息历史 (上下文共享) */

    struct edc_task_group *prev, *next; /* session 内 groups 链表 */
};

/* ── API ────────────────────────────────────────────────────── */

extern struct edc_task_group *edc_task_group_new(struct edc_task *leader);
extern void edc_task_group_free(struct edc_task_group *group);

extern void edc_task_group_add_task(struct edc_task_group *group, struct edc_task *task);
extern void edc_task_group_remove_task(struct edc_task_group *group, struct edc_task *task);
extern struct edc_task *edc_task_group_find_task(struct edc_task_group *group, const char *task_id);

extern void edc_task_group_set_leader(struct edc_task_group *group, struct edc_task *new_leader);
extern void edc_task_group_mark_orphaned(struct edc_task_group *group);

extern void edc_task_group_send_signal(struct edc_task_group *group, edc_task_signal sig);

extern bool edc_task_group_is_foreground_capable(struct edc_task_group *group);
extern const char *edc_task_group_status_label(struct edc_task_group *group);

#endif
