#include "edc_task_group.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "edc_session.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_token_estimate.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

/* The history slot array's only bulk operation; defined with the history section below
 * and needed by edc_task_group_free() at the top. */
static void history_clear(struct edc_task_group *group);

/* ── Lifetime ──────────────────────────────────────────────── */

struct edc_task_group *edc_task_group_new(struct edc_task *leader)
{
    if (!leader)
        return NULL;

    struct edc_task_group *group = g_new0(struct edc_task_group, 1);
    group->group_id = leader->task_id;
    group->leader = leader;
    group->members = leader;
    leader->group_next = NULL;
    group->member_count = 1;
    group->is_orphaned = false;
    /* User group is the default; a system group is only named explicitly by a
     * system task's creator (see edc_group_kind in task_group.h). */
    group->kind = EDC_GROUP_KIND_USER;
    group->history = NULL; /* slots are added as the history is built */
    group->memory = NULL;
    group->fold_files = NULL;
    group->fold_epoch = 0;

    leader->is_group_leader = 1;
    leader->group = group;
    leader->group_id = group->group_id;

    /* The leader starts ALIVE (a fresh task's default), so this group is busy: no idle
     * stamp yet. Recounting here rather than assuming also covers a caller that hands
     * over an already-terminal leader. */
    edc_task_group_refresh_busy(group, time(NULL));

    return group;
}

/* ── "Is anybody still working?" ───────────────────────────── */

void edc_task_group_refresh_busy(struct edc_task_group *group, time_t now)
{
    if (!group)
        return;

    int busy = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue;
        if (!edc_task_is_finished(t))
            busy++;
    }

    const int was = group->busy_members;
    group->busy_members = busy;
    if (busy > 0)
        group->idle_since = 0; /* somebody can still move: the idle clock is not running */
    else if (was > 0 || group->idle_since == 0)
        group->idle_since = now; /* it went idle now, or was first seen idle now */
    /* else: already idle with a stamp -- keep the moment it started, not "still idle". */
}

void edc_task_group_free(struct edc_task_group *group)
{
    if (!group)
        return;

    struct edc_task *task, *tmp;
    LL_FOREACH_SAFE2(group->members, task, tmp, group_next)
    {
        LL_DELETE2(group->members, task, group_next);
        edc_task_free(task);
    }

    history_clear(group);
    if (group->tools_decl)
        cJSON_Delete(group->tools_decl);
    if (group->tool_deltas)
        cJSON_Delete(group->tool_deltas);
    if (group->memory)
        sdsfree(group->memory);
    edc_fold_files_free(group->fold_files);
    free(group);
}

/* ── Kind ──────────────────────────────────────────────────── */

const char *edc_task_group_kind_name(edc_group_kind kind)
{
    switch (kind)
    {
    case EDC_GROUP_KIND_USER:
        return "user";
    case EDC_GROUP_KIND_SYSTEM:
        return "system";
    }
    return "user";
}

edc_group_kind edc_task_group_classify_kind(const char *name)
{
    if (!name)
        return EDC_GROUP_KIND_USER;
    if (strcmp(name, "system") == 0)
        return EDC_GROUP_KIND_SYSTEM;
    return EDC_GROUP_KIND_USER;
}

bool edc_task_group_is_system(const struct edc_task_group *group)
{
    return group && group->kind == EDC_GROUP_KIND_SYSTEM;
}

/* ── Group history ─────────────────────────────────────────── */

/* Append a slot, growing the array. */
static struct edc_history_slot *history_slot_new(struct edc_task_group *group)
{
    if (!group)
        return NULL;
    if (group->history_count == group->history_cap)
    {
        const int ncap = group->history_cap ? group->history_cap * 2 : 16;
        struct edc_history_slot *grown =
            realloc(group->history, (size_t)ncap * sizeof(*grown));
        if (!grown)
            return NULL;
        group->history = grown;
        group->history_cap = ncap;
    }
    struct edc_history_slot *slot = &group->history[group->history_count++];
    slot->msg_id = NULL;
    slot->owned = NULL;
    return slot;
}

static void history_add_id(struct edc_task_group *group, const char *msg_id)
{
    if (!group || !msg_id || !*msg_id)
        return;
    struct edc_history_slot *slot = history_slot_new(group);
    if (!slot)
        return;
    slot->msg_id = sdsnew(msg_id);
}

static void history_clear(struct edc_task_group *group)
{
    if (!group || !group->history)
        return;
    for (int i = 0; i < group->history_count; i++)
    {
        if (group->history[i].msg_id)
            sdsfree(group->history[i].msg_id);
        if (group->history[i].owned)
            cJSON_Delete(group->history[i].owned);
    }
    free(group->history);
    group->history = NULL;
    group->history_count = 0;
    group->history_cap = 0;
}

void edc_task_group_history_add_owned(struct edc_task_group *group, cJSON *msg)
{
    if (!group || !msg)
        return;
    struct edc_history_slot *slot = history_slot_new(group);
    if (!slot)
    {
        cJSON_Delete(msg); /* the caller handed ownership over */
        return;
    }
    slot->owned = msg;
}

void edc_task_group_history_add_turn(struct edc_task_group *group, struct edc_turn *turn)
{
    if (!group || !turn)
        return;

    /* Only the ids: the rows stay in the store and are read when a request actually
     * sends them. This is the difference between a history that costs a pointer per
     * message and one that costs the message. */
    int folded = (turn->folded_prefix > 0) ? turn->folded_prefix : 0;
    if (folded > turn->msg_count)
        folded = turn->msg_count;
    if (folded >= turn->msg_count)
        return;

    struct edc_msg_ids ids;
    if (!edc_msg_ids_for_turn(turn->task_id, turn->turn_ord, folded, turn->msg_count, &ids))
        return;
    for (int i = 0; i < ids.n; i++)
        history_add_id(group, ids.ids[i]);
    edc_msg_ids_free(&ids);
}

int edc_task_group_history_count(struct edc_task_group *group)
{
    return group ? group->history_count : 0;
}

void edc_task_group_history_reset(struct edc_task_group *group)
{
    history_clear(group);
}

cJSON *edc_task_group_history_owned_at(struct edc_task_group *group, int i)
{
    if (!group || i < 0 || i >= group->history_count)
        return NULL;
    return group->history[i].owned;
}

cJSON *edc_task_group_history_at(struct edc_task_group *group, int i)
{
    if (!group || i < 0 || i >= group->history_count)
        return NULL;
    struct edc_history_slot *slot = &group->history[i];
    if (slot->owned)
        return cJSON_Duplicate(slot->owned, 1);
    if (slot->msg_id)
        return edc_msg_get_by_id(slot->msg_id);
    return NULL;
}

cJSON *edc_task_group_history_msgs(struct edc_task_group *group, int from, int to)
{
    if (!group)
        return NULL;
    if (to < 0 || to > group->history_count)
        to = group->history_count;
    if (from < 0)
        from = 0;
    if (to <= from)
        return NULL;

    /* Two passes so the DB-backed slots are fetched in ONE query: with a group whose
     * history is 40k messages, one lookup per slot is 40k round trips. */
    const int n = to - from;
    const char **ids = malloc((size_t)n * sizeof(*ids));
    int *slot_of = malloc((size_t)n * sizeof(*slot_of));
    if (!ids || !slot_of)
    {
        free(ids);
        free(slot_of);
        return NULL;
    }
    int n_ids = 0;
    for (int i = from; i < to; i++)
    {
        if (group->history[i].msg_id)
        {
            ids[n_ids] = group->history[i].msg_id;
            slot_of[n_ids] = i;
            n_ids++;
        }
    }

    cJSON *loaded = n_ids > 0 ? edc_msg_load_by_ids(ids, n_ids) : NULL;
    free(ids);
    free(slot_of);

    /* Alignment guard: `loaded` skips ids whose row vanished between collecting them and
     * this query, and the walk below cannot tell WHICH slot is missing -- it would hand
     * slot i the row that belongs to slot i+1 and every message after it would be off by
     * one. Rare (a row disappears only through truncate/clear, which rebuild the history),
     * but silently shifting a conversation is exactly the failure this whole refactor is
     * trying to make impossible. When the counts disagree, fall back to per-slot lookups. */
    const int n_loaded = loaded ? cJSON_GetArraySize(loaded) : 0;
    if (n_ids > 0 && n_loaded != n_ids)
    {
        if (loaded)
            cJSON_Delete(loaded);
        loaded = NULL;
        cJSON *out_fallback = cJSON_CreateArray();
        for (int i = from; i < to; i++)
        {
            cJSON *msg = edc_task_group_history_at(group, i);
            if (msg)
                cJSON_AddItemToArray(out_fallback, msg);
        }
        if (cJSON_GetArraySize(out_fallback) == 0)
        {
            cJSON_Delete(out_fallback);
            return NULL;
        }
        return out_fallback;
    }

    /* Walk the slots in order, taking from |loaded| only the rows it actually returned
     * (a missing id is skipped rather than leaving a hole in the request). Detaching
     * always takes index 0: detaching from the front shifts everything after it, so a
     * running index would skip every other message. */
    cJSON *out = cJSON_CreateArray();
    for (int i = from; i < to; i++)
    {
        struct edc_history_slot *slot = &group->history[i];
        if (slot->owned)
        {
            cJSON_AddItemToArray(out, cJSON_Duplicate(slot->owned, 1));
            continue;
        }
        if (slot->msg_id && loaded && cJSON_GetArraySize(loaded) > 0)
        {
            cJSON *msg = cJSON_DetachItemFromArray(loaded, 0);
            if (msg)
                cJSON_AddItemToArray(out, msg);
        }
    }
    if (loaded)
        cJSON_Delete(loaded);

    if (cJSON_GetArraySize(out) == 0)
    {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

const char *edc_task_group_history_role(struct edc_task_group *group, int i)
{
    if (!group || i < 0 || i >= group->history_count)
        return NULL;
    struct edc_history_slot *slot = &group->history[i];
    if (slot->owned)
        return cJSON_GetStringValue(cJSON_GetObjectItem(slot->owned, "role"));
    if (slot->msg_id)
    {
        /* Only the role column is read, and the buffer belongs to the group: the caller
         * (the request builder's window start) compares it and drops it immediately. */
        edc_msg_role_by_id(slot->msg_id, group->role_buf, sizeof(group->role_buf));
        return group->role_buf;
    }
    return NULL;
}

int64_t edc_task_group_history_tokens(struct edc_task_group *group, int from, int to){
    if (!group)
        return 0;
    if (to < 0 || to > group->history_count)
        to = group->history_count;
    if (from < 0)
        from = 0;
    if (to <= from)
        return 0;

    const int n = to - from;
    const char **ids = malloc((size_t)n * sizeof(*ids));
    if (!ids)
        return 0;
    int n_ids = 0;
    int64_t owned_tokens = 0;
    for (int i = from; i < to; i++)
    {
        if (group->history[i].msg_id)
            ids[n_ids++] = group->history[i].msg_id;
        else if (group->history[i].owned)
            owned_tokens += edc_token_estimate_msg(group->history[i].owned);
    }
    int64_t db_tokens = n_ids > 0 ? edc_msg_tokens_by_ids(ids, n_ids) : 0;
    free(ids);
    if (db_tokens < 0)
        db_tokens = 0;
    return db_tokens + owned_tokens;
}


struct edc_task **edc_task_group_content_order(struct edc_task_group *group, size_t *n)
{
    if (n)
        *n = 0;
    if (!group)
        return NULL;

    struct edc_task **arr = NULL;
    size_t cnt = 0, cap = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue;
        if (cnt == cap)
        {
            cap = cap ? cap * 2 : 8;
            arr = realloc(arr, cap * sizeof(*arr));
        }
        arr[cnt++] = t;
    }
    /* Unfinished ones go last (their content is newest), keeping insertion order. */
    for (size_t i = 1; i < cnt; i++)
    {
        struct edc_task *key = arr[i];
        uint32_t kseq = edc_task_is_completed(key) ? key->completion_seq : UINT32_MAX;
        size_t j = i;
        while (j > 0)
        {
            struct edc_task *prev = arr[j - 1];
            uint32_t pseq = edc_task_is_completed(prev) ? prev->completion_seq : UINT32_MAX;
            if (pseq <= kseq)
                break;
            arr[j] = prev;
            j--;
        }
        arr[j] = key;
    }
    if (n)
        *n = cnt;
    return arr;
}

void edc_task_group_rebuild_history(struct edc_task_group *group)
{
    if (!group)
        return;

    /* Keep the existing system prompt object: requests used to reference history[0]
     * directly, and other code still holds the pointer it got from
     * edc_context_append_system_part. A tool-set notice is the one thing that must NOT be
     * kept: it can sit at index 0 (a group whose prompt was never built) and keeping it
     * here would duplicate it, because the notices are re-appended below.
     * `toolset_reason` is the marker that identifies one. */
    cJSON *sys0 = NULL;
    if (group->history_count > 0)
    {
        struct edc_history_slot *first = &group->history[0];
        if (first->owned && !cJSON_HasObjectItem(first->owned, "toolset_reason"))
            sys0 = first->owned; /* detached below by taking ownership */
    }
    /* Drop every slot, but do not delete |sys0|: it is about to be re-added. */
    if (group->history)
    {
        for (int i = 0; i < group->history_count; i++)
        {
            if (group->history[i].msg_id)
                sdsfree(group->history[i].msg_id);
            if (group->history[i].owned && group->history[i].owned != sys0)
                cJSON_Delete(group->history[i].owned);
        }
        free(group->history);
        group->history = NULL;
        group->history_count = 0;
        group->history_cap = 0;
    }
    if (sys0)
    {
        edc_task_group_history_add_owned(group, sys0);
    }
    /* A system group carries no system prompt: it holds only one-shot fold calls,
     * whose requests bring their own instruction (edc_llm_request); nothing reads
     * the group history. Building a main prompt here at load time would waste work
     * and leave a misleading slot. */
    else if (group->session && group->session->config && group->kind != EDC_GROUP_KIND_SYSTEM)
    {
        sds prompt = edc_context_build_system_prompt(group->session->config, group->session->workdir);
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", prompt);
        sdsfree(prompt);
        edc_task_group_history_add_owned(group, sys);
    }

    /* Group content order (the same function as fold selection -- they must agree):
     * completed by completion_seq, unfinished last (not yet committed to history but
     * the newest content). sink=GROUP_MEMORY members are skipped: their turn is a
     * fold prompt and would feed the previous prompt into the next fold. */
    size_t n = 0;
    struct edc_task **arr = edc_task_group_content_order(group, &n);
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *task = arr[i];
        for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
            edc_task_group_history_add_turn(group, task->turns.data[qi]);
    }
    free(arr);

    /* Tool-set change notices go last: they are not turns, so every rebuild (a fold landing
     * or a restart) has to put them back. Losing them would leave the model holding a tool
     * that silently stopped existing -- the exact failure this design exists to avoid.
     *
     * Only the LAST one is replayed. Each notice carries a complete `<tools>` section (the
     * whole current declaration set, not a diff), so the newest one already subsumes every
     * earlier one -- and the tag is what lets the model read it as a replacement rather than
     * as an extra opinion. Replaying them all would accumulate a few hundred tokens per
     * change in a request that is re-sent every turn, for information that is already stale. */
    if (group->tool_deltas)
    {
        const int n = cJSON_GetArraySize(group->tool_deltas);
        if (n > 0)
            edc_task_group_history_add_owned(group, cJSON_Duplicate(cJSON_GetArrayItem(group->tool_deltas, n - 1), 1));
    }
}

/* ── Member management ─────────────────────────────────────── */

void edc_task_group_add_task(struct edc_task_group *group, struct edc_task *task)
{
    if (!group || !task)
        return;

    /* already a member of this group */
    if (task->group == group)
        return;

    task->group_id = group->group_id;
    task->group = group;

    LL_APPEND2(group->members, task, group_next);
    group->member_count++;
    edc_task_group_refresh_busy(group, time(NULL));
}

void edc_task_group_remove_task(struct edc_task_group *group, struct edc_task *task)
{
    if (!group || !task)
        return;

    struct edc_task *m, *tmp;
    LL_FOREACH_SAFE2(group->members, m, tmp, group_next)
    {
        if (m == task)
        {
            LL_DELETE2(group->members, m, group_next);
            task->group = NULL;
            group->member_count--;
            edc_task_group_refresh_busy(group, time(NULL));

            if (task == group->leader && group->member_count > 0)
            {
                edc_task_group_mark_orphaned(group);
            }

            return;
        }
    }
}

struct edc_task *edc_task_group_find_task(struct edc_task_group *group, int64_t task_id)
{
    if (!group || task_id <= 0)
        return NULL;

    struct edc_task *task;
    LL_FOREACH2(group->members, task, group_next)
    {
        if (task->task_id == task_id)
            return task;
    }
    return NULL;
}

/* ── Leader management ─────────────────────────────────────── */

void edc_task_group_set_leader(struct edc_task_group *group, struct edc_task *new_leader)
{
    if (!group || !new_leader)
        return;

    if (group->leader)
    {
        group->leader->is_group_leader = 0;
    }
    group->leader = new_leader;
    new_leader->is_group_leader = 1;
    group->is_orphaned = false;

    group->group_id = new_leader->task_id;

    struct edc_task *m;
    LL_FOREACH2(group->members, m, group_next)
    {
        m->group_id = group->group_id;
    }
}

void edc_task_group_mark_orphaned(struct edc_task_group *group)
{
    if (!group)
        return;
    group->is_orphaned = true;
    if (group->leader)
    {
        group->leader->is_group_leader = 0;
        group->leader = NULL;
    }
}

/* ── Signals ───────────────────────────────────────────────── */

/* Per-member signalling is NOT here: signalling also does the host's bookkeeping per
 * member (index / reclaim queue / mark-dirty / settle), and the group does not know
 * the host -- that loop lives in the scheduler (sched_signal_group_members). Only the
 * group's own part remains here, so "host forgot a member's bookkeeping" has one
 * code path, not two. */

/** The group's OWN wrap-up after a group-level signal: leader cancelled -> mark the
 *  group orphaned. (Outside the member roster; only the group module knows it.) */
void edc_task_group_finish_signal(struct edc_task_group *group)
{
    if (!group)
        return;
    if (edc_task_is_cancelled(group->leader) && group->member_count > 0)
    {
        edc_task_group_mark_orphaned(group);
    }
}

/* ── State queries ─────────────────────────────────────────── */

bool edc_task_group_is_foreground_capable(struct edc_task_group *group)
{
    if (!group)
        return false;
    return group->member_count > 0;
}

const char *edc_task_group_status_label(struct edc_task_group *group)
{
    if (!group)
        return "";
    if (group->is_orphaned)
    {
        if (group->member_count == 0)
            return "DEAD";
        return "ORPHANED";
    }
    return "ACTIVE";
}

size_t edc_task_group_count_foldable(struct edc_task_group *group)
{
    if (!group)
        return 0;

    size_t n = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_foldable(t))
            n++;
    }
    return n;
}

size_t edc_task_group_unfolded_msg_count(struct edc_task_group *group)
{
    if (!group)
        return 0;

    /* Count only CONVERSATION tasks' messages (see edc_task_is_foldable):
     * sink=GROUP_MEMORY tasks (the fold task itself) do not participate in folding and
     * must not push the threshold up, or the system group would trigger a pointless
     * fold. */
    size_t total = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue;
        total += edc_task_unfolded_msg_count(t);
    }
    return total;
}
