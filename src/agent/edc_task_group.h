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

    /* 每条 history 消息的归属 task_id, 与 history 下标严格对齐
     * (history[0] 组 system prompt 为 NULL)。history 里放的是各 task turn
     * 消息的引用, 归属信息无法从消息对象本身反推, 故单独记录。不写进消息
     * 对象, 因为同一个对象会作为 LLM 请求体的一部分, 不能携带非标准字段。 */
    sds *history_task_ids;
    size_t history_task_ids_count;
    size_t history_task_ids_cap;

    /* ── L1 折叠 ── */
    sds memory;      /* GROUP 记忆摘要 (已折叠 turns 的浓缩) */
    size_t fold_cursor; /* 已折叠的历史消息数 (history[1+fold_cursor:] 为未折叠历史) */

    struct edc_task_group *prev, *next; /* session 内 groups 链表 */
};

/* ── API ────────────────────────────────────────────────────── */

extern struct edc_task_group *edc_task_group_new(struct edc_task *leader);
extern void edc_task_group_free(struct edc_task_group *group);

extern void edc_task_group_add_task(struct edc_task_group *group, struct edc_task *task);
extern void edc_task_group_remove_task(struct edc_task_group *group, struct edc_task *task);
extern struct edc_task *edc_task_group_find_task(struct edc_task_group *group, const char *task_id);

/* 追加一条消息到组共享历史, 同时记录其归属 task_id。所有对 group->history
 * 的写入都必须走这里, 以保证归属数组与 history 下标对齐。
 *   _owned: 接管 |msg| 所有权 (新构造的消息, 如组 system prompt)
 *   _ref:   仅存引用 (消息归 turn 所有)
 * |task_id| 可为 NULL (无主的组 system prompt)。 */
extern void edc_task_group_history_add_owned(struct edc_task_group *group, cJSON *msg, const char *task_id);
extern void edc_task_group_history_add_ref(struct edc_task_group *group, cJSON *msg, const char *task_id);
/* 返回 history[index] 的归属 task_id, 无归属或下标越界返回 NULL。 */
extern const char *edc_task_group_history_task_id(struct edc_task_group *group, size_t index);

extern void edc_task_group_set_leader(struct edc_task_group *group, struct edc_task *new_leader);
extern void edc_task_group_mark_orphaned(struct edc_task_group *group);

extern void edc_task_group_send_signal(struct edc_task_group *group, edc_task_signal sig);
/** 只对"正在执行"的成员发信号 (edc_task_is_running), 其余成员原样保留。 */
extern void edc_task_group_send_signal_running(struct edc_task_group *group, edc_task_signal sig);

extern bool edc_task_group_is_foreground_capable(struct edc_task_group *group);
extern const char *edc_task_group_status_label(struct edc_task_group *group);

/* 组内可折叠 (COMPLETED && !turns_folded) 的 task 数 */
extern size_t edc_task_group_count_foldable(struct edc_task_group *group);
/* 组内未折叠的消息条数 (history 总消息数 - system prompt - 已折叠) */
extern size_t edc_task_group_unfolded_msg_count(struct edc_task_group *group);

#endif
