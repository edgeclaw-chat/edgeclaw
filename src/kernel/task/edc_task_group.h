#ifndef __BC_TASK_GROUP_H__
#define __BC_TASK_GROUP_H__

#include "edc_task.h"
#include "sds.h"
#include <stdbool.h>

struct edc_session;

/* ── group 的类别 (runtime-model.md §1.4 / §4.6) ───────────────
 *
 * USER = 用户任务的分组 (默认); SYSTEM = 系统任务的分组 (fold 等) —— 侧栏
 * **单独一块**渲染, 好让用户看见有哪些系统任务在跑。
 *
 * **只有展示层可读** —— 这就是防复发的那道闸。老实现用一个 `is_system` 布尔
 * 在 5 处做 skip (`/task list`、侧栏、`is_foldable`、序列化过滤器…), 漏一处就
 * 冒出 `Group system` (既有事故见 MEMORY: 加载期收敛成非瞬时态后过滤器不再丢它)。
 * 新做法相反: system task **照常出现在 `/task list` 与快照里**(可见), 唯一的
 * 区别是展示层把它画到另一块 —— 没有"漏一处"这回事, 因为**没有地方在隐藏它**。 */
typedef enum
{
    EDC_GROUP_KIND_USER,   /* 用户任务的分组 (默认) */
    EDC_GROUP_KIND_SYSTEM, /* 系统任务的分组 */
} edc_group_kind;

extern const char *edc_task_group_kind_name(edc_group_kind kind);
extern edc_group_kind edc_task_group_classify_kind(const char *name);
/** 是否是系统分组。读者: 展示层 (画在哪一块) + **容器自己** (session 用它回答
 *  "我的孩子里哪个是系统组" —— 结构性问题, 属内聚不属耦合)。core 不得据此
 * 隐藏/跳过任何东西 (真越界只有一种形态: 非容器模块拿它改行为)。 */
extern bool edc_task_group_is_system(const struct edc_task_group *group);

/* ── Task Group (Process Group, PGID) ──────────────────────────── */

struct edc_task_group
{
    char *group_id;           /* PGID (= leader 的 task_id) */
    struct edc_task *leader;  /* group leader task */
    struct edc_task *members; /* 链表头: group 内所有 task (含 leader) */
    int member_count;
    struct edc_session *session; /* 属主 session (由 edc_session_add_group 绑定) */

    edc_group_kind kind; /* USER (默认) / SYSTEM —— 只有展示层可读, 见上面的 enum */

    bool is_orphaned; /* leader 退出后置 true */

    cJSON *history; /* 组内消息历史 (上下文共享) */

    /* ── L1 折叠 ── */
    sds memory;         /* GROUP 记忆摘要 (已折叠 turns 的浓缩) */
    uint32_t folded_through_completion_seq; /* 折叠水位: completion_seq ≤ 它的 task 已折进 memory */

    struct edc_task_group *prev, *next; /* session 内 groups 链表 */
};

/* ── API ────────────────────────────────────────────────────── */

extern struct edc_task_group *edc_task_group_new(struct edc_task *leader);
extern void edc_task_group_free(struct edc_task_group *group);

extern void edc_task_group_add_task(struct edc_task_group *group, struct edc_task *task);
extern void edc_task_group_remove_task(struct edc_task_group *group, struct edc_task *task);
extern struct edc_task *edc_task_group_find_task(struct edc_task_group *group, const char *task_id);

/* 追加一条消息到组共享历史。所有对 group->history 的写入都必须走这里。
 *   _owned: 接管 |msg| 所有权 (新构造的消息, 如组 system prompt)
 *   _ref:   仅存引用 (消息归 turn 所有) */
extern void edc_task_group_history_add_owned(struct edc_task_group *group, cJSON *msg);
extern void edc_task_group_history_add_ref(struct edc_task_group *group, cJSON *msg);

extern void edc_task_group_set_leader(struct edc_task_group *group, struct edc_task *new_leader);
extern void edc_task_group_mark_orphaned(struct edc_task_group *group);

/* 逐成员发信号在 scheduler (它要顺带给每个成员做宿主簿记); 组里只留自己那部分
 * 收尾 —— leader 被取消则组标记孤儿。 */
extern void edc_task_group_finish_signal(struct edc_task_group *group);

extern bool edc_task_group_is_foreground_capable(struct edc_task_group *group);
extern const char *edc_task_group_status_label(struct edc_task_group *group);

/* 组内可折叠 (COMPLETED && !turns_folded) 的 task 数 */
extern size_t edc_task_group_count_foldable(struct edc_task_group *group);
/* 组内未折叠的消息条数 (成员 turns 消息总数 - 已折叠) */
extern size_t edc_task_group_unfolded_msg_count(struct edc_task_group *group);
/* 已折叠的消息条数 = 组历史中 system prompt 之后的前缀长度
 * (history 是各成员 turns 按 completion_seq 升序拼接, 故前缀 = 已折叠 task 的消息) */
extern size_t edc_task_group_folded_msg_count(struct edc_task_group *group);

#endif
