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
    group->history_task_ids = NULL;
    group->history_task_ids_count = 0;
    group->history_task_ids_cap = 0;
    group->memory = NULL;
    group->fold_cursor = 0;

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
    if (group->history_task_ids)
    {
        for (size_t i = 0; i < group->history_task_ids_count; i++)
        {
            if (group->history_task_ids[i])
                sdsfree(group->history_task_ids[i]);
        }
        free(group->history_task_ids);
    }
    if (group->memory)
        sdsfree(group->memory);
    free(group);
}

/* ── 组历史 (含归属) ──────────────────────────────────────── */

/* 记录归属: 扩容后追加, 与 history 下标对齐。 */
static void group_history_record(struct edc_task_group *group, const char *task_id)
{
    if (group->history_task_ids_count == group->history_task_ids_cap)
    {
        size_t cap = group->history_task_ids_cap ? group->history_task_ids_cap * 2 : 8;
        group->history_task_ids = realloc(group->history_task_ids, cap * sizeof(sds));
        group->history_task_ids_cap = cap;
    }
    group->history_task_ids[group->history_task_ids_count++] = task_id ? sdsnew(task_id) : NULL;
}

void edc_task_group_history_add_owned(struct edc_task_group *group, cJSON *msg, const char *task_id)
{
    if (!group || !msg)
        return;
    if (!group->history)
        group->history = cJSON_CreateArray();
    cJSON_AddItemToArray(group->history, msg);
    group_history_record(group, task_id);
}

void edc_task_group_history_add_ref(struct edc_task_group *group, cJSON *msg, const char *task_id)
{
    if (!group || !msg)
        return;
    if (!group->history)
        group->history = cJSON_CreateArray();
    cJSON_AddItemReferenceToArray(group->history, msg);
    group_history_record(group, task_id);
}

const char *edc_task_group_history_task_id(struct edc_task_group *group, size_t index)
{
    if (!group || !group->history_task_ids || index >= group->history_task_ids_count)
        return NULL;
    return group->history_task_ids[index];
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

    if (group->leader && group->leader->state == TASK_CANCELLED && group->member_count > 0)
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

size_t edc_task_group_unfolded_msg_count(struct edc_task_group *group)
{
    if (!group || !group->history)
        return 0;

    size_t total = cJSON_GetArraySize(group->history);
    /* history[0] 是 system prompt, 不计入历史消息; 剩余减去已折叠消息 */
    size_t hist_msgs = (total > 0) ? total - 1 : 0;
    if (group->fold_cursor >= hist_msgs)
        return 0;
    return hist_msgs - group->fold_cursor;
}
