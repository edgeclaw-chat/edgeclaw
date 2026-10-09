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

/* Fold policy (pi alignment, decisions 13/14). A group folds when its estimated
 * context exceeds `context_window - EDC_FOLD_RESERVE_TOKENS`; the fold keeps the
 * newest EDC_FOLD_KEEP_RECENT_TOKENS of detail and everything older becomes the
 * summarization corpus. The reserve is what has to cover the summary request's
 * own overhead *and* its answer, hence the cap below. */
#define EDC_FOLD_RESERVE_TOKENS 16384
#define EDC_FOLD_KEEP_RECENT_TOKENS 20000
#define EDC_FOLD_SUMMARY_MAX_TOKENS ((EDC_FOLD_RESERVE_TOKENS * 8) / 10)

/* L1 fold: turns of finished tasks in a group -> group->memory, run as a system
 * task { sink = GROUP_MEMORY } in the session's system group. This module decides
 * whether to fold, what to fold, and dispatches; the scheduler lands the output. */

/** Gate (decision 6/14/15): a group is due when it has foldable tasks and
 *
 *     context_window > 0  ->  ctx > context_window - EDC_FOLD_RESERVE_TOKENS
 *     context_window <= 0 ->  unfolded messages >= group_fold_msg_threshold
 *
 *  The window axis is **exclusive**, not a disjunction: 0 means "not configured /
 *  unknown" and is the only value that reaches the count axis. Do not give
 *  `context_window` a non-zero default -- any default would make the count branch
 *  unreachable (a fallback that only fires when a key is absent dies silently). */
extern bool edc_group_fold_due(struct edc_task_group *group, const struct edc_config *conf);

/** Scan every session (so a background session folds too, not only when switched
 *  back to) and start at most one fold. Returns whether one was started; when it
 *  did, `*out_session` (optional) is the session whose group was folded, so the
 *  caller can persist the new fold task and its dep edges at once. */
extern bool edc_group_fold_try(struct edc_scheduler *sched, struct edc_session_manager *sessions,
                               struct edc_config *conf, struct edc_session **out_session);

/** Where one round's cut went (P2a-4): the member it falls in, that member's turn
 *  index, and how many leading messages of that turn the memory covers. Everything
 *  earlier in content order is folded whole; nothing later is touched. A NULL task
 *  id means no cut (a fold task written by an older binary). */
/** 切点落在成员的**在飞 turn** 内部时的 turn 标记: 它还没有下标。落地时解析成
 *  active_turn; 若这期间它已提交 (成员跑到 turn 尾就结束了), 就退到队列末尾 —— 同一个
 *  对象, 所以用指针比较而不是序号 (提交会让序号漂移)。 */
#define EDC_FOLD_CUT_ACTIVE_TURN 0x7fffffffu

struct edc_fold_cut
{
    const char *task_id;
    uint32_t turn;
    uint32_t prefix;
};

/** Build the fold prompt and the cut it corresponds to (they are computed together,
 *  from one pass, so the corpus sent and the prefix applied can never disagree).
 *  Selection (pi's keep_recent): walk the group's unfolded messages in content order
 *  from the newest backwards, keep the newest EDC_FOLD_KEEP_RECENT_TOKENS as detail,
 *  everything older is the corpus. The cut lands on a message boundary. When the whole
 *  group is under that budget -- only reachable when no window is configured -- fold
 *  the oldest half instead, or a small group could never fold. */
/** |focus| (optional, NULL ok) lands verbatim in the prompt as "Additional focus:" --
 *  it is how the manual `/group fold [id] [focus]` (decision 8) steers a fold. */
extern sds edc_group_fold_build_prompt(struct edc_task_group *group, const char *focus, struct edc_fold_cut *out_cut);

/** Start a fold: create a system task carrying the target group and watermark,
 *  attach the prompt, dispatch it. NULL when one is already in flight, when the
 *  group is not quiescent (a member is still running), when there is nothing to
 *  fold, or when dispatch failed. The prompt follows pi's two-part layout (see
 *  kInitialSummaryInstruction): a plain fold passes NULL focus. */
extern struct edc_task *edc_group_fold_start(struct edc_scheduler *sched, struct edc_session *session,
                                             struct edc_task_group *group, const char *focus);

/** Barrier read by the scheduler: true while a fold targeting |group| is in
 *  flight. The scheduler then refuses to drive any task of that group (the fold
 *  task itself lives in the session's system group, so it is not held by its own
 *  fold). Together with edc_group_fold_start refusing a non-quiescent group, this
 *  serializes the group's work strictly around a fold. */
extern bool edc_group_fold_holds_group(struct edc_task_group *group);

/** Landing half: write |content| into task->sink_group's memory, apply the cut the
 *  fold task carries (every member before it folded whole, its own turn up to
 *  fold_cut_prefix), rebuild the group history, bump fold_epoch. On failure nothing
 *  changes and the next round retries. */
extern void edc_group_fold_deliver_memory(struct edc_task *task, const char *content);

#endif
