#ifndef __BC_TASK_GROUP_H__
#define __BC_TASK_GROUP_H__

#include "edc_task.h"
#include "sds.h"
#include <stdbool.h>

struct edc_session;

/* USER groups user tasks (default); SYSTEM groups system tasks (fold), rendered as
 * a separate sidebar block. This kind is readable by the display layer only: system
 * tasks still show up in `/task list` and snapshots, just in another block, so
 * there is no skip anywhere that could be forgotten. */
typedef enum
{
    EDC_GROUP_KIND_USER,
    EDC_GROUP_KIND_SYSTEM,
} edc_group_kind;

extern const char *edc_task_group_kind_name(edc_group_kind kind);
extern edc_group_kind edc_task_group_classify_kind(const char *name);
/** True for a system group. Read by the display layer and by the container itself
 *  (a session asking which of its children is the system group); core must not hide
 *  or skip anything based on it. */
extern bool edc_task_group_is_system(const struct edc_task_group *group);

struct edc_task_group
{
    char *group_id;           /* PGID (= the leader's task_id) */
    struct edc_task *leader;
    struct edc_task *members; /* all tasks in the group, including the leader */
    int member_count;
    struct edc_session *session; /* owner, bound by edc_session_add_group */

    edc_group_kind kind; /* display layer only */

    bool is_orphaned; /* set once the leader exits */

    cJSON *history; /* shared message history */

    /* L1 fold state: the summarized memory. What has been folded is tracked per
     * turn (edc_turn.folded_prefix) — the history below only ever holds what is
     * still sent, so no group-level watermark is needed. */
    sds memory;

    /* Bumped every time a fold lands. A member's context anchor (the last reply's
     * real usage) only counts the prefix that was sent *then*; once a fold has
     * replaced that prefix, the anchor measures a context that no longer exists,
     * so it is ignored until the member replies again. */
    uint32_t fold_epoch;

    struct edc_task_group *prev, *next; /* session's group list */
};

extern struct edc_task_group *edc_task_group_new(struct edc_task *leader);
extern void edc_task_group_free(struct edc_task_group *group);

extern void edc_task_group_add_task(struct edc_task_group *group, struct edc_task *task);
extern void edc_task_group_remove_task(struct edc_task_group *group, struct edc_task *task);
extern struct edc_task *edc_task_group_find_task(struct edc_task_group *group, const char *task_id);

/* Append a message to the group's shared history; every write must go through
 * these. `_owned` takes ownership, `_ref` stores a borrowed reference. */
extern void edc_task_group_history_add_owned(struct edc_task_group *group, cJSON *msg);
extern void edc_task_group_history_add_ref(struct edc_task_group *group, cJSON *msg);
/* Append a committed turn's *unfolded* messages (turn->folded_prefix onwards).
 * The history exists to say what will be sent, so folded messages never enter. */
extern void edc_task_group_history_add_turn(struct edc_task_group *group, struct edc_turn *turn);
/* Rebuild the history the one canonical way: system prompt + every member's
 * committed turns in completion_seq order, minus their folded prefixes. Used by
 * the fold landing and by load, so the array can never drift from the turns. */
extern void edc_task_group_rebuild_history(struct edc_task_group *group);

/* 组内容序 —— 折叠选料与 group history 共用的**唯一**顺序 (两者必须一致, 否则"折掉的
 * 前缀"就不是"请求里读到的那段前缀")。成员里 sink=GROUP_MEMORY 的跳过; 已完成的按
 * completion_seq 升序; 没完成的排在最后 (组内插入序) —— 未完成任务的 completion_seq 是
 * 0, 直接按数值排会把它顶到最前面, 而它的内容恰恰是最新的。返回新数组 (调用方 free),
 * 条数经 *n 返回。 */
extern struct edc_task **edc_task_group_content_order(struct edc_task_group *group, size_t *n);

extern void edc_task_group_set_leader(struct edc_task_group *group, struct edc_task *new_leader);
extern void edc_task_group_mark_orphaned(struct edc_task_group *group);

/* Per-member signalling lives in the scheduler (it also owes each member host
 * bookkeeping); only the group's own part remains here. */
extern void edc_task_group_finish_signal(struct edc_task_group *group);

extern bool edc_task_group_is_foreground_capable(struct edc_task_group *group);
extern const char *edc_task_group_status_label(struct edc_task_group *group);

/* Foldable members (COMPLETED && still holding unfolded messages). */
extern size_t edc_task_group_count_foldable(struct edc_task_group *group);
/* Unfolded messages in the group: members' committed turn messages minus each
 * turn's folded prefix (sink=GROUP_MEMORY members excluded). */
extern size_t edc_task_group_unfolded_msg_count(struct edc_task_group *group);

#endif
