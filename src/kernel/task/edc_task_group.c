#include "edc_task_group.h"
#include "edc_compat.h"
#include "edc_context.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

/* ── Lifetime ──────────────────────────────────────────────── */

struct edc_task_group *edc_task_group_new(struct edc_task *leader)
{
    if (!leader)
        return NULL;

    struct edc_task_group *group = g_new0(struct edc_task_group, 1);
    group->group_id = strdup(leader->task_id);
    group->leader = leader;
    group->members = leader;
    leader->group_next = NULL;
    group->member_count = 1;
    group->is_orphaned = false;
    /* User group is the default; a system group is only named explicitly by a
     * system task's creator (see edc_group_kind in task_group.h). */
    group->kind = EDC_GROUP_KIND_USER;
    group->history = cJSON_CreateArray();
    group->memory = NULL;
    group->fold_epoch = 0;

    leader->is_group_leader = 1;
    leader->group = group;
    free(leader->group_id);
    leader->group_id = strdup(group->group_id);

    return group;
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

    free(group->group_id);
    if (group->history)
        cJSON_Delete(group->history);
    if (group->tools_decl)
        cJSON_Delete(group->tools_decl);
    if (group->tool_deltas)
        cJSON_Delete(group->tool_deltas);
    if (group->memory)
        sdsfree(group->memory);
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

void edc_task_group_history_add_owned(struct edc_task_group *group, cJSON *msg)
{
    if (!group || !msg)
        return;
    if (!group->history)
        group->history = cJSON_CreateArray();
    cJSON_AddItemToArray(group->history, msg);
}

void edc_task_group_history_add_ref(struct edc_task_group *group, cJSON *msg)
{
    if (!group || !msg)
        return;
    if (!group->history)
        group->history = cJSON_CreateArray();
    cJSON_AddItemReferenceToArray(group->history, msg);
}

void edc_task_group_history_add_turn(struct edc_task_group *group, struct edc_turn *turn)
{
    if (!group || !turn)
        return;

    size_t len = edc_queue_length(&turn->messages);
    size_t folded = (turn->folded_prefix > 0) ? (size_t)turn->folded_prefix : 0;
    if (folded > len)
        folded = len;
    for (size_t i = folded; i < len; i++)
        edc_task_group_history_add_ref(group, turn->messages.data[turn->messages.head + i]);
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

    /* Keep the existing system prompt object: requests add history[0] by
     * reference, so replacing the object would strand the ones in flight.
     * A tool-set notice is the one thing that must NOT be kept: it can sit at index 0 (a
     * group whose prompt was never built) and keeping it here would duplicate it, because
     * the notices are re-appended below. `toolset_reason` is the marker that identifies one. */
    cJSON *sys0 = NULL;
    if (group->history && cJSON_GetArraySize(group->history) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(group->history, 0);
        if (first && !cJSON_HasObjectItem(first, "toolset_reason"))
            sys0 = cJSON_DetachItemFromArray(group->history, 0);
        cJSON_Delete(group->history);
    }
    group->history = cJSON_CreateArray();
    if (sys0)
    {
        cJSON_AddItemToArray(group->history, sys0);
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
     * that silently stopped existing -- the exact failure this design exists to avoid. */
    if (group->tool_deltas)
    {
        cJSON *delta = NULL;
        cJSON_ArrayForEach(delta, group->tool_deltas)
            edc_task_group_history_add_owned(group, cJSON_Duplicate(delta, 1));
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

    free(task->group_id);
    task->group_id = strdup(group->group_id);
    task->group = group;

    LL_APPEND2(group->members, task, group_next);
    group->member_count++;
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

            if (task == group->leader && group->member_count > 0)
            {
                edc_task_group_mark_orphaned(group);
            }

            return;
        }
    }
}

struct edc_task *edc_task_group_find_task(struct edc_task_group *group, const char *task_id)
{
    if (!group || !task_id)
        return NULL;

    struct edc_task *task;
    LL_FOREACH2(group->members, task, group_next)
    {
        if (strcmp(task->task_id, task_id) == 0)
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

    free(group->group_id);
    group->group_id = strdup(new_leader->task_id);

    struct edc_task *m;
    LL_FOREACH2(group->members, m, group_next)
    {
        free(m->group_id);
        m->group_id = strdup(group->group_id);
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
