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

/* L1 fold: turns of finished tasks in a group -> group->memory, run as a system
 * task { sink = GROUP_MEMORY } in the session's system group. This module decides
 * whether to fold, what to fold, and dispatches; the scheduler lands the output. */

/** Gate: a group is due when it has foldable tasks and its unfolded message count
 *  reached the threshold. */
extern bool edc_group_fold_due(struct edc_task_group *group, int msg_threshold);

/** Scan every session (so a background session folds too, not only when switched
 *  back to) and start at most one fold. Returns whether one was started. */
extern bool edc_group_fold_try(struct edc_scheduler *sched, struct edc_session_manager *sessions,
                               struct edc_config *conf);

/** Build the fold prompt (half strategy: current memory + the oldest half of the
 *  foldable tasks' turns and output); `*out_max_seq` returns the batch ceiling. */
extern sds edc_group_fold_build_prompt(struct edc_task_group *group, uint32_t *out_max_seq);

/** Start a fold: create a system task carrying the target group and watermark,
 *  attach the prompt, dispatch it. NULL when one is already in flight, when the
 *  group is not quiescent (a member is still running), when there is nothing to
 *  fold, or when dispatch failed. */
extern struct edc_task *edc_group_fold_start(struct edc_scheduler *sched, struct edc_session *session,
                                             struct edc_task_group *group);

/** Barrier read by the scheduler: true while a fold targeting |group| is in
 *  flight. The scheduler then refuses to drive any task of that group (the fold
 *  task itself lives in the session's system group, so it is not held by its own
 *  fold). Together with edc_group_fold_start refusing a non-quiescent group, this
 *  serializes the group's work strictly around a fold. */
extern bool edc_group_fold_holds_group(struct edc_task_group *group);

/** Landing half: write |content| into task->sink_group's memory, mark this round's
 *  members (completion_seq <= task->fold_max_seq) folded, advance the watermark. On
 *  failure nothing changes and the next round retries. */
extern void edc_group_fold_deliver_memory(struct edc_task *task, const char *content);

#endif
