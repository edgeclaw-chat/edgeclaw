#include "edc_task_group.h"
#include "edc_compat.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

/* ── 生命周期 ──────────────────────────────────────────────── */

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
    group->history = cJSON_CreateArray();
    group->memory = NULL;
    group->folded_through_completion_seq = 0;

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
    if (group->memory)
        sdsfree(group->memory);
    free(group);
}

/* ── 组历史 ───────────────────────────────────────────────── */

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

/* ── 成员管理 ──────────────────────────────────────────────── */

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

/* ── Leader 管理 ───────────────────────────────────────────── */

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

/* ── 信号 ──────────────────────────────────────────────────── */

static void group_send_signal_impl(struct edc_task_group *group, edc_task_signal sig, bool running_only)
{
    if (!group)
        return;

    struct edc_task *task, *tmp;
    LL_FOREACH_SAFE2(group->members, task, tmp, group_next)
    {
        if (running_only && !edc_task_is_running(task))
            continue;
        edc_task_send_signal(task, sig);
    }

    if (edc_task_is_cancelled(group->leader) && group->member_count > 0)
    {
        edc_task_group_mark_orphaned(group);
    }
}

void edc_task_group_send_signal(struct edc_task_group *group, edc_task_signal sig)
{
    group_send_signal_impl(group, sig, false);
}

void edc_task_group_send_signal_running(struct edc_task_group *group, edc_task_signal sig)
{
    group_send_signal_impl(group, sig, true);
}

/* ── 状态查询 ──────────────────────────────────────────────── */

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

size_t edc_task_group_folded_msg_count(struct edc_task_group *group)
{
    if (!group)
        return 0;

    size_t n = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        /* completion_seq == 0 的 task 尚未提交, 永不算已折叠。 */
        if (t->completion_seq > 0 && t->completion_seq <= group->folded_through_completion_seq)
            n += edc_task_turn_msg_count(t);
    }
    return n;
}

size_t edc_task_group_unfolded_msg_count(struct edc_task_group *group)
{
    if (!group)
        return 0;

    size_t total = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
        total += edc_task_turn_msg_count(t);

    size_t folded = edc_task_group_folded_msg_count(group);
    return total > folded ? total - folded : 0;
}
