#ifndef __BC_TASK_GROUP_H__
#define __BC_TASK_GROUP_H__

#include "edc_fold_files.h"
#include "edc_task.h"
#include "sds.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

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
    int64_t group_id;         /* PGID (= the leader's task_id) */
    struct edc_task *leader;
    struct edc_task *members; /* all tasks in the group, including the leader */
    int member_count;
    struct edc_session *session; /* owner, bound by edc_session_add_group */

    edc_group_kind kind; /* display layer only */

    bool is_orphaned; /* set once the leader exits */

    /* ── The group's shared history: IDS, not copies ──────────────────────
     *
     * This used to be a cJSON array holding a COPY of every unfolded message, which for a
     * 40k-message session meant 164 MiB resident for a conversation that is already on
     * disk. It is now a slot list: a message is either an id (read on demand) or an object
     * that has no row at all (the system prompt in slot 0, the tool-change notice at the
     * tail). Order is the array order -- that IS the request order.
     *
     * The whole point is that a request pays for the window it sends, not for the history
     * it has. */
    struct edc_history_slot *history;
    int history_count;
    int history_cap;
    /* Scratch for edc_task_group_history_role(): the role string of the last slot asked
     * about. A buffer here instead of an allocation because that lookup runs on every
     * request to decide where its window starts. */
    char role_buf[32];

    /* The declaration set this group's requests send -- WHICH tools the model is told about
     * (frozen for the group's lifetime). Resolved once on first use from the
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
     * still sent, so no group-level watermark is needed.
     *
     * `memory` is the summary BODY only. The `<read-files>`/`<modified-files>` blocks the
     * model sees are rendered from `fold_files` on every request; keeping them out of the
     * text is what stops a summary that rewrites a block from corrupting the list. */
    sds memory;
    struct edc_fold_files *fold_files;

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

    /* ── "Is anybody still working?" -- the clock the L2 extraction windows run on ──
     *
     * `busy_members` counts the members that can still advance on their own: not
     * terminal and not a memory sink (a fold/curation round is not user work, and the
     * extraction predicate must not deadlock on its own siblings). 0 = the group is
     * idle, i.e. nobody is running.
     *
     * `idle_since` is the wall-clock moment that count last DROPPED TO zero (epoch
     * seconds; 0 = "not known to be idle"). The extraction predicate measures its idle
     * window from here, never from the newest message's timestamp: a task that runs for
     * fifteen minutes and is then stopped leaves a group that became idle only NOW,
     * while the newest message is fifteen minutes old -- the message clock called that
     * "long silent" and started a round while the user was typing their next line.
     *
     * Both fields are derived and in-memory: edc_task_group_refresh_busy() recounts from
     * the member list on every terminal transition, on (de)attachment and once per
     * extraction scan, so no writer has to keep a delta in step and a load that restores
     * a different set cannot leave them stale. A restart stamps `idle_since` afresh --
     * the same "a restart counts as new" rule as fold_failures above. */
    int busy_members;
    time_t idle_since;

    struct edc_task_group *prev, *next; /* session's group list */
};

extern struct edc_task_group *edc_task_group_new(struct edc_task *leader);
extern void edc_task_group_free(struct edc_task_group *group);

extern void edc_task_group_add_task(struct edc_task_group *group, struct edc_task *task);
extern void edc_task_group_remove_task(struct edc_task_group *group, struct edc_task *task);
extern struct edc_task *edc_task_group_find_task(struct edc_task_group *group, int64_t task_id);

/* Recount busy_members from the member list and (re)stamp or clear idle_since.
 *
 * Call it after anything that can move a member between "can still advance" and
 * terminal -- a life change (wired into edc_task_move), an attachment, or a load -- with
 * |now| = time(NULL) at the call site. Idempotent and O(members): the recount is the
 * source of truth, so a missed notification self-heals on the next call instead of
 * leaving the two fields out of step. The stamp only moves when the group crosses into
 * idle (or is seen idle for the first time); staying idle keeps the original moment. */
extern void edc_task_group_refresh_busy(struct edc_task_group *group, time_t now);

/* ── Group history: a slot list, not a copy of the conversation ───────────────
 *
 * Each slot is either a message ID (the row lives in the store and is read on demand) or
 * an object kept in memory because it has no row at all: the system prompt in slot 0, and
 * the tool-change notice at the tail. The array order is the request order.
 *
 * Readers get what they need without materializing the whole history:
 *   count     -> edc_task_group_history_count()
 *   one msg   -> edc_task_group_history_at()      (caller owns)
 *   a range   -> edc_task_group_history_msgs()    (one query for the range)
 *   a total   -> edc_task_group_history_tokens()  (SUM over the rows)
 */
struct edc_history_slot
{
    sds msg_id;   /* non-NULL: a message row, loaded on demand */
    cJSON *owned; /* non-NULL: an in-memory message (system prompt / tool notice) */
};

/* Append a message the caller owns and that has no row (the system prompt, a
 * tool-change notice). */
extern void edc_task_group_history_add_owned(struct edc_task_group *group, cJSON *msg);
/* Append a committed turn's *unfolded* messages (turn->folded_prefix onwards). Only their
 * ids are stored; the history exists to say what will be sent, so folded messages never
 * enter. */
extern void edc_task_group_history_add_turn(struct edc_task_group *group, struct edc_turn *turn);

/* Drop every slot. The history is DERIVED (rebuilt from the turns), so this is what a
 * restart leaves behind before rebuild_history runs; a caller that wants the content back
 * must rebuild. */
extern void edc_task_group_history_reset(struct edc_task_group *group);

/* The OWNED object at slot |i|, borrowed (NULL when the slot is a DB-backed id or past the
 * end). Exists for in-place mutation of an in-memory message -- /reload rewrites the
 * system prompt's content on the SAME node, because requests hold references to it and a
 * replaced node would strand them. Everywhere else, use history_at() and own the copy. */
extern cJSON *edc_task_group_history_owned_at(struct edc_task_group *group, int i);

/* How many messages the history holds (in-memory slots included). */
extern int edc_task_group_history_count(struct edc_task_group *group);
/* Message |i| as a fresh object the CALLER owns (cJSON_Delete it), or NULL past the end. */
extern cJSON *edc_task_group_history_at(struct edc_task_group *group, int i);
/* [from, to) as a fresh array the caller owns; |to| < 0 means "to the end". One query for
 * the whole range -- this is the request assembly path. */
extern cJSON *edc_task_group_history_msgs(struct edc_task_group *group, int from, int to);
/* Sum of the token estimates of [from, to); |to| < 0 means "to the end". */
extern int64_t edc_task_group_history_tokens(struct edc_task_group *group, int from, int to);

/* The ROLE of message |i| ("user" / "assistant" / "tool" / "system"), or NULL.
 * A borrowed pointer that stays valid until the slot changes. Exists so the request
 * builder can ask "is this a tool result?" while deciding where a window starts without
 * paying for the whole message -- that check runs on every request. */
extern const char *edc_task_group_history_role(struct edc_task_group *group, int i);

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
