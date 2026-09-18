#ifndef __BC_GROUP_FOLD_H__
#define __BC_GROUP_FOLD_H__

#include "sds.h"
#include <stdbool.h>

struct edc_scheduler;
struct edc_session;
struct edc_session_manager;
struct edc_config;
struct edc_task;
struct edc_task_group;

/* ── L1 折叠: group 内已终结 task 的 turns → group->memory ──────
 *
 * 旧实现 (memory_actor) 是一个**专用 actor + 专用协议**: scheduler 回调发
 * BC_ACTOR_MSG_GROUP_FOLD 给它, 它自己发 LLM 请求, 回来再回一发 GROUP_FOLD_DONE
 * 让 scheduler 落盘。P0b 之后它就是一个 **system task**:
 *
 *   { sink = GROUP_MEMORY, lifetime = TRANSIENT }   —— 放进 session 的 system group
 *
 * —— 有真活干 (一次折叠 LLM 调用)、不落盘 (TRANSIENT, 完成即回收)、可见
 * (侧栏单独一块, 不需要任何 skip)。产出的去处由 sink 决定 (executor 分流到
 * ops->deliver_group_memory), 落地在 scheduler: 写 memory、标已折叠、推水位。
 *
 * 这里只负责"要不要折、折什么、把任务派出去"三件事。 */

/** 触发闸门: 这个组该折了吗 —— 有可折叠任务, 且未折叠消息数达到阈值。 */
extern bool edc_group_fold_due(struct edc_task_group *group, int msg_threshold);

/** 触发扫描: 现在该折哪个组 —— 扫**所有** session (后台 session 里的任务完成后
 *  也要能触发, 否则只有切回来才折), 一次只折一个组。返回是否发起了折叠。
 *
 *  这是它唯一的入口。它原来住装配根、经 scheduler 的 `on_task_settled` 端口被调:
 *  端口那层只递进来两样本模块拿不到的东西 —— session 仓库与 config (阈值), 别的
 *  全是本模块自己做的事, 所以端口去掉、逻辑归位到这里 (见 runtime-model.md §11)。 */
extern bool edc_group_fold_try(struct edc_scheduler *sched, struct edc_session_manager *sessions,
                               struct edc_config *conf);

/** 构造折叠 prompt (半量策略: 现有 memory + 最老的一半可折叠任务的 turns 与
 *  output; `*out_max_seq` 回传选中批次的 completion_seq 上限)。
 *  没有可折叠任务时返回 NULL。 */
extern sds edc_group_fold_build_prompt(struct edc_task_group *group, uint32_t *out_max_seq);

/** 发起一次折叠: 建一个 system task (挂上目标组与水位)、把 prompt 装成它的
 *  input、派发它。返回该任务; 该 session 已有折叠在飞 / 没有可折叠内容 /
 *  派发失败时返回 NULL。 */
extern struct edc_task *edc_group_fold_start(struct edc_scheduler *sched, struct edc_session *session,
                                             struct edc_task_group *group);

/** 折叠产出的**落地**(接收半边): 把 |content| 写进 task->sink_group 的记忆、
 *  把本轮选中 (completion_seq <= task->fold_max_seq) 的已终结成员标成已折叠、
 *  推进折叠水位。失败 (无目标组 / 空内容) 时**什么都不改**, 下一轮再折。
 *
 *  它只碰 group 自己的字段 (memory / folded_through_completion_seq / 成员的
 *  turns_folded), 不需要任何宿主 —— 所以归 fold 模块, 不再是 executor 的 ops
 *  端口 (原来叫 sched_deliver_group_memory, 住在 scheduler)。 */
extern void edc_group_fold_deliver_memory(struct edc_task *task, const char *content);

#endif
