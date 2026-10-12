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
 * context exceeds `context_window - EDC_FOLD_RESERVE_TOKENS` (the window is always
 * configured, see edc_init_env.h); the fold keeps the newest
 * EDC_FOLD_KEEP_RECENT_TOKENS of detail and everything older becomes the
 * summarization corpus. */
#define EDC_FOLD_RESERVE_TOKENS 16384
#define EDC_FOLD_KEEP_RECENT_TOKENS 20000

/* (a) The summary's OUTPUT budget.
 *
 * It is tuned to the corpus (see EDC_FOLD_MAX_CORPUS_TOKENS) rather than derived
 * from the trigger margin: the corpus can be the whole history outside
 * keep_recent, and a budget too small for it made every call hit
 * finish_reason=length, so the fold never landed and the group retried unchanged
 * forever. */
#define EDC_FOLD_SUMMARY_MAX_TOKENS 32768

/* (b) Corpus cap per fold, in our estimate units.
 *
 * Keeps the compression ratio achievable (<= 4:1) instead of pushing 10:1+ onto
 * the summarizer. Over the cap the cut moves back -- keep more detail, fold less,
 * fold the rest next round; the cut is always a message boundary. */
#define EDC_FOLD_MAX_CORPUS_TOKENS (EDC_FOLD_SUMMARY_MAX_TOKENS * 4)

/* (c) Consecutive-failure cap.
 *
 * At the cap automatic folding STOPS (manual `/group fold` is exempt), so "fail ->
 * group still due -> retry unchanged" cannot become a perpetual motion machine.
 * The count resets on successful landing / manual fold / new committed content;
 * starting a fold does NOT reset it. In-memory only, not persisted -- a restart
 * counts as new. */
#define EDC_FOLD_MAX_FAILURES 3

/* L1 fold: turns of finished tasks in a group -> group->memory, run as a system
 * task { sink = GROUP_MEMORY } in the session's system group. This module decides
 * whether to fold, what to fold, and dispatches; the scheduler lands the output. */

/** Gate (decision 6): a group is due when it has foldable tasks and
 *
 *     ctx > context_window - EDC_FOLD_RESERVE_TOKENS
 *
 *  The token axis is the **only** axis (2026-10-10: the message-count branch was
 *  deleted). `context_window` is always positive -- edc_init_env repairs a missing
 *  or <= 0 value to EDC_CONFIG_DEFAULT_CONTEXT_WINDOW -- so the count branch that
 *  used to catch "window unknown" was unreachable code, and a fallback that only
 *  fires when a key is absent dies silently as soon as anything supplies a default.
 *  If a bad config still yields <= 0 here we refuse to fold automatically rather
 *  than guess; the manual `/group fold` never asks this predicate. */
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
/** Turn marker for a cut that falls inside a member's IN-FLIGHT turn (it has no
 *  index yet). Resolved to active_turn at landing; if the turn committed meanwhile
 *  it falls back to the end of the queue. The same object, so it is compared by
 *  pointer, not index (commit shifts indices). */
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
 *  group is under that budget (a group whose summary is bigger than what is left to
 *  fold, or a manual fold of a small group) fold the oldest half instead, or a small
 *  group could never fold. The corpus is then
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
/* Fold landing: write the group memory (and advance the prefix / fold_epoch where
 * possible). Returns true when the memory was really written (L2's "new material"
 * predicate is this return value); false when the content was empty and nothing
 * changed (retry on the next trigger). */
extern bool edc_group_fold_deliver_memory(struct edc_task *task, const char *content);

/** Record one fold failure (task ERROR / empty summary / unresolvable cut). At
 *  EDC_FOLD_MAX_FAILURES, `edc_group_fold_due` returns false on the AUTOMATIC
 *  path; manual `/group fold` is unaffected. */
extern void edc_group_fold_note_failure(struct edc_task_group *group);

/** Reset the consecutive-failure count. Three call sites: successful landing,
 *  manual fold, and new committed content in the group (a new turn = new
 *  situation, worth retrying). Starting a fold does NOT reset it. */
extern void edc_group_fold_note_progress(struct edc_task_group *group);

#endif
