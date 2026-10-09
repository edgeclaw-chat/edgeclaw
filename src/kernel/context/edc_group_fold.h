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
 * summarization corpus. */
#define EDC_FOLD_RESERVE_TOKENS 16384
#define EDC_FOLD_KEEP_RECENT_TOKENS 20000

/* (a) 摘要的**输出**预算.
 *
 * 旧值 = 0.8 x reserve = 13107 —— 那是"窗口里留白"的量 (触发线的余量), 不是"摘要
 * 该有多大"的量。语料可以是 keep_recent 之外的**整段历史**: 2026-10-09 实测 178 条
 * 消息 / ~137k token, 要模型把 137k 压进 13k, 结果每发都撞 finish_reason=length;
 * 而截断按 P2b-2 被丢弃 ⇒ 折叠永远不落地 ⇒ 组一直"该折", 每 ~40s 原样重试一发
 * (每发都是一次 ~67k prompt 的真调用), 组里的成员被反复 park。
 *
 * 现在预算与语料配套 (见 EDC_FOLD_MAX_CORPUS_TOKENS), 取一个够宽敞的常数。 */
#define EDC_FOLD_SUMMARY_MAX_TOKENS 32768

/* (b) 单次折叠送进摘要器的**语料上限** (我们的估算口径)。
 *
 * 保证语料与输出预算之间的压缩比可达成 (≤ 4:1), 而不是把 10:1 甚至更高的压缩比
 * 压给摘要器。超上限时把切点往回收 —— 多留细节、少折一点, 下一轮再折剩下的; 切点
 * 永远还是消息边界 (与 keep_recent 同一条规则)。 */
#define EDC_FOLD_MAX_CORPUS_TOKENS (EDC_FOLD_SUMMARY_MAX_TOKENS * 4)

/* (c) 连续失败的次数上限。
 *
 * 到顶就**停自动折叠** (手动 `/group fold` 不受此限), 免得"失败 → 组仍然该折 →
 * 原样重试"变成永动机。计数在**落地成功 / 手动折叠 / 组里又有了已提交内容**时清零
 * (后两者才是"新情况"; 起折本身不清零, 否则永远到不了上限)。只在内存, 不落盘 ——
 * 重启视为新情况。 */
#define EDC_FOLD_MAX_FAILURES 3

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
 *  the oldest half instead, or a small group could never fold. The corpus is then
 *  capped at EDC_FOLD_MAX_CORPUS_TOKENS (b): the cut moves back until what is sent can
 *  actually be compressed into EDC_FOLD_SUMMARY_MAX_TOKENS in one call. */
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

/** The system message every fold call carries (pi §3: the summary is a separate
 *  one-shot call, so it must forbid continuing or answering the transcript).
 *  The fold request is **self-contained**: this message + the fold prompt, and
 *  nothing else. It deliberately does not live in a group's history[0]: that slot
 *  is not persisted, and after a restart the loader refilled it with the agent's
 *  own system prompt — which then rode along in every fold call. */
extern const char *edc_group_fold_system_prompt(void);

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

/** (c) 记一次折叠失败 (任务进 ERROR / 摘要为空 / 切点解析不了)。计数到
 *  EDC_FOLD_MAX_FAILURES 后 `edc_group_fold_due` 对**自动**路径返回 false; 手动
 *  `/group fold` 不受影响。 */
extern void edc_group_fold_note_failure(struct edc_task_group *group);

/** (c) 清零连续失败计数。三个调用点: 折叠落地成功、手动折叠、组里又有了已提交内容
 *  (新的 turn = 新情况, 值得再试)。**起折不清零** —— 否则永远到不了上限。 */
extern void edc_group_fold_note_progress(struct edc_task_group *group);

#endif
