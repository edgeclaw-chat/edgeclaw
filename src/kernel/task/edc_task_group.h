#ifndef __BC_TASK_GROUP_H__
#define __BC_TASK_GROUP_H__

#include "edc_task.h"
#include "sds.h"
#include <stdbool.h>
#include <stdint.h>

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

    /* The declaration set this group's requests send -- WHICH tools the model is told about
     * (docs/pi-toolset-plan.zh.md, I2 "组内冻结"). Resolved once on first use from the
     * registry + the transcript's deltas, and recomputed only when a change is non-additive
     * (a removal or a redefinition). Not persisted: both halves are reproducible (the
     * registry from config, the deltas from `history`), and history[0] is not persisted
     * either -- see the note in edc_group_fold.c about that slot being refilled on load.
     * `toolset_epoch` counts real changes, for logs and cache-miss attribution. */
    cJSON *tools_decl;
    uint32_t toolset_epoch;

    /* "Tool set changed" notices, one system message per change. They are NOT turns, and
     * `history` is rebuilt from turns (fold landing / restart), so they live here and
     * rebuild_history re-appends them at the tail. In memory only, on purpose: the set
     * itself is rebuilt from the registry + config, which is the durable source of truth;
     * the notices only explain a change to the model within this process's lifetime. */
    cJSON *tool_deltas;

    /* L1 fold state: the summarized memory. What has been folded is tracked per
     * turn (edc_turn.folded_prefix) — the history below only ever holds what is
     * still sent, so no group-level watermark is needed. */
    sds memory;

    /* Wall-clock about `memory` (epoch seconds; 0 = unknown / never written).
     * `memory_created_at` = first write, `memory_updated_at` = the last write (i.e.
     * "how fresh is the group memory"), `memory_covers_to` = the newest message
     * timestamp among the messages the memory now covers ("up to when does it know").
     * The last one is what a curator needs to judge whether new material is newer or
     * older than the memory. Set by the fold landing; persisted. */
    uint64_t memory_created_at;
    uint64_t memory_updated_at;
    uint64_t memory_covers_to;

    /* Bumped every time a fold lands. A member's context anchor (the last reply's
     * real usage) only counts the prefix that was sent *then*; once a fold has
     * replaced that prefix, the anchor measures a context that no longer exists,
     * so it is ignored until the member replies again. */
    uint32_t fold_epoch;

    /* Consecutive failure count: at EDC_FOLD_MAX_FAILURES, automatic folding stops,
     * so "fail -> group still due -> retry unchanged" cannot become a perpetual
     * motion machine. Reset on successful landing / manual fold / new committed
     * content. In-memory only, not persisted (a restart counts as new). */
    int fold_failures;

    /* Same shape for the L2 extraction of this group's unfolded history: consecutive
     * failures stop the AUTOMATIC path at EDC_MEMO_EXTRACT_MAX_FAILURES; reset on
     * successful landing / new committed content. In-memory only. */
    int memo_extract_failures;

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

/* Group content order -- the ONE order shared by fold selection and group history
 * (they must agree, or "the folded prefix" is not "the prefix the request read").
 * Members with sink=GROUP_MEMORY are skipped; completed ones by ascending
 * completion_seq; unfinished ones last (in-group insertion order) -- an unfinished
 * task's completion_seq is 0, so a plain numeric sort would hoist the newest
 * content to the front. Returns a new array (caller frees); count via *n. */
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
