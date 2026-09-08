#include "edc_state_serializer.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_containers.h"
#include "edc_context.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <string.h>

/* ══════════════════════════════════════════════════════════════
 *  Internal helpers — dep type ↔ string
 *  ══════════════════════════════════════════════════════════════ */

static const char *dep_type_name(edc_task_dep_type t)
{
    switch (t)
    {
    case TASK_DEP_JOIN:
        return "JOIN";
    case TASK_DEP_PIPE:
        return "PIPE";
    case TASK_DEP_SEQUENCE:
        return "SEQUENCE";
    default:
        return "UNKNOWN";
    }
}

static edc_task_dep_type dep_type_from_name(const char *name)
{
    if (!name)
        return TASK_DEP_JOIN;
    if (strcmp(name, "JOIN") == 0)
        return TASK_DEP_JOIN;
    if (strcmp(name, "PIPE") == 0)
        return TASK_DEP_PIPE;
    if (strcmp(name, "SEQUENCE") == 0)
        return TASK_DEP_SEQUENCE;
    return TASK_DEP_JOIN;
}

/* ══════════════════════════════════════════════════════════════
 *  Helpers: find message index in session->messages by pointer
 *  ══════════════════════════════════════════════════════════════ */

/** Return the index of `msg` in `messages_array`, or -1 if not found. */
static int find_msg_index(cJSON *messages_array, cJSON *msg)
{
    size_t n = cJSON_GetArraySize(messages_array);
    for (size_t i = 0; i < n; i++)
    {
        if (cJSON_GetArrayItem(messages_array, i) == msg)
            return (int)i;
    }
    return -1;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single turn
 *
 *  Strategy: first try to locate each message in session->messages
 *  by pointer.  If every message is found, write a compact
 *  message_start / message_count pair (indices into the
 *  messages array).  Otherwise embed the message content
 *  directly in a "messages" array as a fallback.
 *  ══════════════════════════════════════════════════════════════ */

static cJSON *turn_to_json(struct edc_turn *turn, cJSON *messages_array)
{
    cJSON *tj = cJSON_CreateObject();
    size_t msg_count = edc_queue_length(&turn->messages);

    /* Phase 3: if the turn has a recorded range (from edc_session_add_turn),
     * use it directly — this is the common path for flushed turns.
     * The messages are already in session->messages at the recorded indices. */
    if (turn->range_valid && turn->msg_range_count > 0)
    {
        cJSON_AddNumberToObject(tj, "message_start", (int)turn->msg_range_start);
        cJSON_AddNumberToObject(tj, "message_count", (int)turn->msg_range_count);
        return tj;
    }

    /* Fallback for turns that still have messages in their queue
     * (not yet flushed via edc_session_add_turn). */

    if (msg_count == 0)
    {
        cJSON_AddNumberToObject(tj, "message_start", 0);
        cJSON_AddNumberToObject(tj, "message_count", 0);
        return tj;
    }

    size_t head = turn->messages.head;
    size_t tail = turn->messages.tail;

    /* Try pointer-based lookup first */
    bool all_found = true;
    int first_idx = -1;
    for (size_t i = head; i < tail; i++)
    {
        cJSON *msg = (cJSON *)turn->messages.data[i];
        int idx = find_msg_index(messages_array, msg);
        if (idx < 0)
        {
            all_found = false;
            break;
        }
        if (first_idx < 0)
            first_idx = idx;
    }

    if (all_found)
    {
        /* Compact: just the range */
        cJSON_AddNumberToObject(tj, "message_start", first_idx);
        cJSON_AddNumberToObject(tj, "message_count", (int)msg_count);
    }
    else
    {
        /* Fallback: embed messages directly */
        cJSON *embedded = cJSON_CreateArray();
        for (size_t i = head; i < tail; i++)
        {
            cJSON *msg = (cJSON *)turn->messages.data[i];
            cJSON_AddItemToArray(embedded, cJSON_Duplicate(msg, 1));
        }
        cJSON_AddItemToObject(tj, "messages", embedded);
    }

    return tj;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single task
 *  ══════════════════════════════════════════════════════════════ */

static cJSON *task_to_json(struct edc_task *task, cJSON *messages_array)
{
    cJSON *tj = cJSON_CreateObject();

    /* ── identity ── */
    cJSON_AddStringToObject(tj, "task_id", task->task_id ? task->task_id : "");
    cJSON_AddStringToObject(tj, "group_id", task->group_id ? task->group_id : "");
    cJSON_AddStringToObject(tj, "session_id", task->session_id ? task->session_id : "");

    /* ── description & state ── */
    cJSON_AddStringToObject(tj, "description", task->description ? task->description : "");
    cJSON_AddStringToObject(tj, "state", edc_task_state_name(task->state));
    cJSON_AddBoolToObject(tj, "is_group_leader", task->is_group_leader);
    cJSON_AddBoolToObject(tj, "auto_reap", task->auto_reap);

    /* ── prompt ── */
    if (task->prompt_name)
        cJSON_AddStringToObject(tj, "prompt_name", task->prompt_name);
    if (task->prompt)
        cJSON_AddStringToObject(tj, "prompt", task->prompt);

    /* ── channel ── */
    cJSON *ch = edc_channel_type_to_json(task->channel_type);
    if (ch)
        cJSON_AddItemToObject(tj, "channel", ch);

    /* ── fork chain ── */
    if (task->forked_from)
        cJSON_AddStringToObject(tj, "forked_from_task_id", task->forked_from->task_id);
    cJSON_AddNumberToObject(tj, "fork_point_msg_count", (int)task->fork_point_msg_count);

    /* ── deps (outgoing only) ── */
    cJSON *deps = cJSON_CreateArray();
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "source_task_id", dep->source ? dep->source->task_id : "");
        cJSON_AddStringToObject(d, "type", dep_type_name(dep->type));
        cJSON_AddBoolToObject(d, "completed", dep->completed);
        cJSON_AddBoolToObject(d, "cancelled", dep->cancelled);
        cJSON_AddBoolToObject(d, "injected", dep->injected);
        cJSON_AddItemToArray(deps, d);
    }
    cJSON_AddItemToObject(tj, "deps", deps);

    /* ── output ── */
    if (task->output)
        cJSON_AddStringToObject(tj, "output", task->output);
    cJSON_AddNumberToObject(tj, "completion_seq", task->completion_seq);
    cJSON_AddBoolToObject(tj, "turns_folded", task->turns_folded);

    /* ── redirect ── */
    if (task->redirect_stdout)
        cJSON_AddStringToObject(tj, "redirect_stdout", task->redirect_stdout);
    if (task->redirect_stderr)
        cJSON_AddStringToObject(tj, "redirect_stderr", task->redirect_stderr);
    cJSON_AddBoolToObject(tj, "redirect_append", task->redirect_append);

    /* ── todolist ── */
    if (task->todolist)
        cJSON_AddItemToObject(tj, "todolist", cJSON_Duplicate(task->todolist, 1));
    /* todo 同步节流计数 (距上次 update_todolist 的 LLM 请求数, 跨会话保持) */
    cJSON_AddNumberToObject(tj, "todo_sync_gap", task->todo_sync_gap);

    /* ── turns (committed) ── */
    cJSON *turns_json = cJSON_CreateArray();
    {
        size_t head = task->turns.head;
        size_t tail = task->turns.tail;
        int turn_idx = 0;
        for (size_t i = head; i < tail; i++)
        {
            struct edc_turn *t = (struct edc_turn *)task->turns.data[i];
            cJSON *tj_inner = turn_to_json(t, messages_array);
            cJSON_AddNumberToObject(tj_inner, "turn_index", turn_idx++);
            cJSON_AddItemToArray(turns_json, tj_inner);
        }
    }
    cJSON_AddItemToObject(tj, "turns", turns_json);

    /* ── active_turn ── */
    if (task->active_turn)
    {
        cJSON *at = turn_to_json(task->active_turn, messages_array);
        cJSON_AddNumberToObject(at, "turn_index", -1);
        cJSON_AddItemToObject(tj, "active_turn", at);
    }

    return tj;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single group
 *  ══════════════════════════════════════════════════════════════ */

static cJSON *group_to_json(struct edc_task_group *group)
{
    cJSON *gj = cJSON_CreateObject();

    cJSON_AddStringToObject(gj, "group_id", group->group_id ? group->group_id : "");
    /* leader_task_id: for normal groups use leader->task_id;
     * for orphaned groups fall back to group_id (original leader's id) */
    if (group->leader)
        cJSON_AddStringToObject(gj, "leader_task_id", group->leader->task_id);
    else if (group->group_id)
        cJSON_AddStringToObject(gj, "leader_task_id", group->group_id);
    cJSON_AddBoolToObject(gj, "is_orphaned", group->is_orphaned);
    if (group->memory)
        cJSON_AddStringToObject(gj, "memory", group->memory);
    cJSON_AddNumberToObject(gj, "fold_cursor", (int)group->fold_cursor);

    /* member task ids */
    cJSON *members = cJSON_CreateArray();
    struct edc_task *m;
    LL_FOREACH2(group->members, m, group_next)
    {
        cJSON_AddItemToArray(members, cJSON_CreateString(m->task_id ? m->task_id : ""));
    }
    cJSON_AddItemToObject(gj, "member_task_ids", members);

    return gj;
}

/* ══════════════════════════════════════════════════════════════
 *  Public: serialize
 *  ══════════════════════════════════════════════════════════════ */

cJSON *edc_state_serialize(struct edc_session *session, struct edc_scheduler *sched)
{
    if (!session)
        return NULL;
    if (!session->messages)
    {
        log_error("edc_state_serialize: session->messages is NULL");
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);

    /* ── session block ── */
    cJSON *sj = cJSON_CreateObject();
    cJSON_AddStringToObject(sj, "session_id", session->session_id ? session->session_id : "");
    cJSON_AddStringToObject(sj, "name", session->name ? session->name : "");
    cJSON_AddStringToObject(sj, "created_at", session->created_at ? session->created_at : "");
    cJSON_AddStringToObject(sj, "last_active", session->last_active ? session->last_active : "");
    if (session->active_task)
        cJSON_AddStringToObject(sj, "active_task_id", session->active_task->task_id);
    if (sched && sched->foreground_group)
        cJSON_AddStringToObject(sj, "foreground_group_id", sched->foreground_group->group_id);
    cJSON_AddNumberToObject(sj, "next_completion_seq", (int)session->next_completion_seq);
    cJSON_AddNumberToObject(sj, "message_count", (int)cJSON_GetArraySize(session->messages));
    cJSON_AddItemToObject(root, "session", sj);

    /* ── groups block ── */
    cJSON *groups_json = cJSON_CreateArray();
    {
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
        {
            if (g->member_count == 0)
                continue; /* skip empty */
            cJSON_AddItemToArray(groups_json, group_to_json(g));
        }
    }
    cJSON_AddItemToObject(root, "groups", groups_json);

    /* ── tasks block ── */
    cJSON *tasks_json = cJSON_CreateArray();
    {
        cJSON *messages = session->messages;
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                /* filter transient states: 刚创建未开始的 IDLE 任务不落盘。
                 * CANCELLED 必须持久化 —— 用户显式 stop 的任务要跨重启保留,
                 * 否则重启后会按旧状态恢复 (如 LLM_PROCESSING 降级 WAITING_INPUT),
                 * 让取消看起来"没生效"。 */
                if (t->state == TASK_IDLE)
                    continue;
                cJSON_AddItemToArray(tasks_json, task_to_json(t, messages));
            }
        }
    }
    cJSON_AddItemToObject(root, "tasks", tasks_json);

    return root;
}

/* ══════════════════════════════════════════════════════════════
 *  Deserialize helpers
 *  ══════════════════════════════════════════════════════════════ */

/**
 * Degrade runtime-only states to a restorable state.
 */
static edc_task_state degrade_state(edc_task_state s)
{
    switch (s)
    {
    case TASK_LLM_PROCESSING:
    case TASK_TOOL_CALLING:
        return TASK_WAITING_INPUT;
    default:
        return s;
    }
}

/**
 * Rebuild a turn from JSON.  Two modes:
 *   1. Compact:  message_start + message_count → slice session->messages
 *   2. Embedded: "messages" array → use directly (fallback)
 *
 * Returns a new edc_turn with messages and timestamps populated.
 */
static struct edc_turn *turn_from_json(cJSON *tj, cJSON *messages_array)
{
    struct edc_turn *turn = edc_turn_new();

    /* ── Mode 2: embedded messages (fallback) ── */
    cJSON *embedded = cJSON_GetObjectItem(tj, "messages");
    if (embedded && cJSON_IsArray(embedded))
    {
        size_t n = cJSON_GetArraySize(embedded);
        for (size_t i = 0; i < n; i++)
        {
            cJSON *msg = cJSON_GetArrayItem(embedded, i);
            cJSON *copy = cJSON_Duplicate(msg, 1);
            edc_queue_push(&turn->messages, copy);

            cJSON *ts_obj = cJSON_GetObjectItem(msg, "timestamp");
            sds ts = ts_obj && cJSON_IsString(ts_obj) ? sdsnew(cJSON_GetStringValue(ts_obj)) : sdsempty();
            edc_queue_push(&turn->timestamps, ts);
        }
        return turn;
    }

    /* ── Mode 1: compact range ── */
    int start = cJSON_GetObjectItem(tj, "message_start")
                    ? (int)cJSON_GetNumberValue(cJSON_GetObjectItem(tj, "message_start"))
                    : 0;
    int count = cJSON_GetObjectItem(tj, "message_count")
                    ? (int)cJSON_GetNumberValue(cJSON_GetObjectItem(tj, "message_count"))
                    : 0;

    if (count <= 0 || start < 0)
        return turn;

    size_t total = cJSON_GetArraySize(messages_array);
    for (int i = 0; i < count; i++)
    {
        size_t idx = (size_t)(start + i);
        if (idx >= total)
            break;

        cJSON *msg = cJSON_GetArrayItem(messages_array, idx);
        cJSON *copy = cJSON_Duplicate(msg, 1);
        edc_queue_push(&turn->messages, copy);

        cJSON *ts_obj = cJSON_GetObjectItem(msg, "timestamp");
        sds ts = ts_obj && cJSON_IsString(ts_obj) ? sdsnew(cJSON_GetStringValue(ts_obj)) : sdsempty();
        edc_queue_push(&turn->timestamps, ts);
    }

    return turn;
}

/* ── Simple string→task hash map for deserialization ── */

struct task_map_entry
{
    char *task_id;
    struct edc_task *task;
    struct task_map_entry *next; /* singly-linked collision chain */
};

#define TASK_MAP_BUCKETS 64

static unsigned int hash_str(const char *s)
{
    unsigned int h = 5381;
    while (*s)
        h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}

static void task_map_put(struct task_map_entry **map, const char *id, struct edc_task *task)
{
    unsigned int b = hash_str(id) % TASK_MAP_BUCKETS;
    struct task_map_entry *e = g_new0(struct task_map_entry, 1);
    e->task_id = strdup(id);
    e->task = task;
    e->next = map[b];
    map[b] = e;
}

static struct edc_task *task_map_get(struct task_map_entry **map, const char *id)
{
    if (!id)
        return NULL;
    unsigned int b = hash_str(id) % TASK_MAP_BUCKETS;
    struct task_map_entry *e = map[b];
    while (e)
    {
        if (strcmp(e->task_id, id) == 0)
            return e->task;
        e = e->next;
    }
    return NULL;
}

static void task_map_free(struct task_map_entry **map)
{
    for (int i = 0; i < TASK_MAP_BUCKETS; i++)
    {
        struct task_map_entry *e = map[i];
        while (e)
        {
            struct task_map_entry *next = e->next;
            free(e->task_id);
            free(e);
            e = next;
        }
    }
}

/* ── Simple string→group hash map ── */

struct group_map_entry
{
    char *group_id;
    struct edc_task_group *group;
    struct group_map_entry *next;
};

#define GROUP_MAP_BUCKETS 16

static void group_map_put(struct group_map_entry **map, const char *id, struct edc_task_group *group)
{
    unsigned int b = hash_str(id) % GROUP_MAP_BUCKETS;
    struct group_map_entry *e = g_new0(struct group_map_entry, 1);
    e->group_id = strdup(id);
    e->group = group;
    e->next = map[b];
    map[b] = e;
}

static struct edc_task_group *group_map_get(struct group_map_entry **map, const char *id)
{
    if (!id)
        return NULL;
    unsigned int b = hash_str(id) % GROUP_MAP_BUCKETS;
    struct group_map_entry *e = map[b];
    while (e)
    {
        if (strcmp(e->group_id, id) == 0)
            return e->group;
        e = e->next;
    }
    return NULL;
}

static void group_map_free(struct group_map_entry **map)
{
    for (int i = 0; i < GROUP_MAP_BUCKETS; i++)
    {
        struct group_map_entry *e = map[i];
        while (e)
        {
            struct group_map_entry *next = e->next;
            free(e->group_id);
            free(e);
            e = next;
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Public: deserialize
 *  ══════════════════════════════════════════════════════════════ */

static void deser_rebuild_group_history(struct edc_session *session, struct edc_scheduler *sched)
{
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        if (session->config)
        {
            sds prompt = edc_context_build_system_prompt(session->config);
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", prompt);
            cJSON_AddItemToArray(g->history, sys);
            sdsfree(prompt);
        }

        /* 按 completion_seq 升序重建 history，与运行时 (STOP/完成顺序) 保持一致。
         * 成员链表是创建顺序，若任务完成顺序与创建顺序不同，直接遍历会导致
         * fold_cursor 错位，进而让切片切断 assistant(tool_calls)+tool 序列。 */
        size_t n = 0, cap = 0;
        struct edc_task **arr = NULL;
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (n == cap)
            {
                cap = cap ? cap * 2 : 8;
                arr = realloc(arr, cap * sizeof(*arr));
            }
            arr[n++] = t;
        }

        for (size_t i = 1; i < n; i++)
        {
            struct edc_task *key = arr[i];
            size_t j = i;
            while (j > 0 && arr[j - 1]->completion_seq > key->completion_seq)
            {
                arr[j] = arr[j - 1];
                j--;
            }
            arr[j] = key;
        }

        for (size_t i = 0; i < n; i++)
        {
            struct edc_task *task = arr[i];
            for (size_t qi = task->turns.head; qi < task->turns.tail; qi++)
            {
                struct edc_turn *turn = task->turns.data[qi];
                for (size_t mj = turn->messages.head; mj < turn->messages.tail; mj++)
                    cJSON_AddItemReferenceToArray(g->history, turn->messages.data[mj]);
            }
        }
        free(arr);
    }

    if (sched)
    {
        uint64_t max_id = 0;
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                uint64_t id = strtoull(t->task_id, NULL, 10);
                if (id > max_id)
                    max_id = id;
            }
        }
        if (max_id >= (uint64_t)sched->next_task_id)
            sched->next_task_id = (int64_t)(max_id + 1);
    }
}

static void deser_session_meta(struct edc_session *session, cJSON *sj)
{
    if (!sj)
        return;
    cJSON *name_j = cJSON_GetObjectItem(sj, "name");
    if (name_j && cJSON_IsString(name_j))
    {
        if (session->name)
            sdsfree(session->name);
        session->name = sdsnew(cJSON_GetStringValue(name_j));
    }
    cJSON *la_j = cJSON_GetObjectItem(sj, "last_active");
    if (la_j && cJSON_IsString(la_j))
    {
        if (session->last_active)
            sdsfree(session->last_active);
        session->last_active = sdsnew(cJSON_GetStringValue(la_j));
    }
    cJSON *ncs_j = cJSON_GetObjectItem(sj, "next_completion_seq");
    if (ncs_j && cJSON_IsNumber(ncs_j))
        session->next_completion_seq = (uint32_t)cJSON_GetNumberValue(ncs_j);
}

static void deser_create_tasks(cJSON *json, struct task_map_entry **id_map)
{
    cJSON *tasks_json = cJSON_GetObjectItem(json, "tasks");
    if (!tasks_json || !cJSON_IsArray(tasks_json))
        return;

    size_t tcount = cJSON_GetArraySize(tasks_json);
    for (size_t i = 0; i < tcount; i++)
    {
        cJSON *tj = cJSON_GetArrayItem(tasks_json, i);
        if (!tj)
            continue;
        const char *task_id = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "task_id"));
        if (!task_id)
            continue;

        cJSON *ch_json = cJSON_GetObjectItem(tj, "channel");
        edc_channel_type ch_type = ch_json ? edc_channel_type_from_json(ch_json) : edc_channel_adapter;

        struct edc_task *task = edc_task_new(task_id, cJSON_GetStringValue(cJSON_GetObjectItem(tj, "session_id")) ?: "",
                                             cJSON_GetStringValue(cJSON_GetObjectItem(tj, "group_id")) ?: "",
                                             cJSON_GetStringValue(cJSON_GetObjectItem(tj, "description")) ?: "", ch_type);
        if (!task)
            continue;

        task->state = degrade_state(edc_task_classify_state(cJSON_GetStringValue(cJSON_GetObjectItem(tj, "state"))));

        cJSON *v;
        if ((v = cJSON_GetObjectItem(tj, "is_group_leader")))
            task->is_group_leader = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "auto_reap")))
            task->auto_reap = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "prompt_name")) && cJSON_IsString(v))
            task->prompt_name = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "prompt")) && cJSON_IsString(v))
            task->prompt = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "fork_point_msg_count")) && cJSON_IsNumber(v))
            task->fork_point_msg_count = (size_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "output")) && cJSON_IsString(v))
            task->output = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "completion_seq")) && cJSON_IsNumber(v))
            task->completion_seq = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "turns_folded")))
            task->turns_folded = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "redirect_stdout")) && cJSON_IsString(v))
            task->redirect_stdout = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "redirect_stderr")) && cJSON_IsString(v))
            task->redirect_stderr = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "redirect_append")))
            task->redirect_append = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "todolist")) && cJSON_IsArray(v))
            task->todolist = cJSON_Duplicate(v, 1);
        if ((v = cJSON_GetObjectItem(tj, "todo_sync_gap")) && cJSON_IsNumber(v))
            task->todo_sync_gap = (int)cJSON_GetNumberValue(v);

        task_map_put(id_map, task_id, task);
    }
}

static void deser_fork_chains(cJSON *json, struct task_map_entry **id_map)
{
    cJSON *tasks_json = cJSON_GetObjectItem(json, "tasks");
    if (!tasks_json || !cJSON_IsArray(tasks_json))
        return;

    size_t tcount = cJSON_GetArraySize(tasks_json);
    for (size_t i = 0; i < tcount; i++)
    {
        cJSON *tj = cJSON_GetArrayItem(tasks_json, i);
        const char *task_id = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "task_id"));
        const char *fork_p = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "forked_from_task_id"));
        if (task_id && fork_p)
        {
            struct edc_task *task = task_map_get(id_map, task_id);
            struct edc_task *parent = task_map_get(id_map, fork_p);
            if (task && parent)
                task->forked_from = parent;
        }
    }
}

static void deser_create_groups(cJSON *json, struct edc_session *session, struct task_map_entry **id_map,
                                struct group_map_entry **group_map)
{
    cJSON *groups_json = cJSON_GetObjectItem(json, "groups");
    if (!groups_json || !cJSON_IsArray(groups_json))
        return;

    size_t gcount = cJSON_GetArraySize(groups_json);
    for (size_t i = 0; i < gcount; i++)
    {
        cJSON *gj = cJSON_GetArrayItem(groups_json, i);
        const char *group_id = cJSON_GetStringValue(cJSON_GetObjectItem(gj, "group_id"));
        const char *leader_id = cJSON_GetStringValue(cJSON_GetObjectItem(gj, "leader_task_id"));
        cJSON *orph = cJSON_GetObjectItem(gj, "is_orphaned");
        bool is_orph = orph && cJSON_IsTrue(orph);

        struct edc_task *leader = task_map_get(id_map, leader_id);
        if (!leader && is_orph)
        {
            cJSON *members = cJSON_GetObjectItem(gj, "member_task_ids");
            if (members && cJSON_IsArray(members) && cJSON_GetArraySize(members) > 0)
                leader = task_map_get(id_map, cJSON_GetStringValue(cJSON_GetArrayItem(members, 0)));
        }
        if (!leader)
        {
            log_warn("deserialize: group %s leader %s not found", group_id ?: "(null)", leader_id ?: "(null)");
            continue;
        }

        struct edc_task_group *group = edc_task_group_new(leader);
        if (!group)
            continue;
        if (is_orph)
            group->is_orphaned = true;

        {
            cJSON *mem = cJSON_GetObjectItem(gj, "memory");
            if (mem && cJSON_IsString(mem))
                group->memory = sdsnew(cJSON_GetStringValue(mem));
        }
        {
            cJSON *fc = cJSON_GetObjectItem(gj, "fold_cursor");
            if (fc && cJSON_IsNumber(fc))
                group->fold_cursor = (size_t)cJSON_GetNumberValue(fc);
        }

        cJSON *members = cJSON_GetObjectItem(gj, "member_task_ids");
        if (members && cJSON_IsArray(members))
        {
            size_t mcount = cJSON_GetArraySize(members);
            for (size_t j = 0; j < mcount; j++)
            {
                const char *mid = cJSON_GetStringValue(cJSON_GetArrayItem(members, j));
                if (!mid || strcmp(mid, leader->task_id) == 0)
                    continue;
                struct edc_task *mt = task_map_get(id_map, mid);
                if (mt)
                    edc_task_group_add_task(group, mt);
            }
        }

        edc_session_add_group(session, group);
        if (group_id)
            group_map_put(group_map, group_id, group);
    }
}

static void deser_deps(cJSON *json, struct task_map_entry **id_map)
{
    cJSON *tasks_json = cJSON_GetObjectItem(json, "tasks");
    if (!tasks_json || !cJSON_IsArray(tasks_json))
        return;

    size_t tcount = cJSON_GetArraySize(tasks_json);
    for (size_t i = 0; i < tcount; i++)
    {
        cJSON *tj = cJSON_GetArrayItem(tasks_json, i);
        const char *task_id = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "task_id"));
        cJSON *deps_json = cJSON_GetObjectItem(tj, "deps");
        if (!task_id || !deps_json || !cJSON_IsArray(deps_json))
            continue;

        struct edc_task *target = task_map_get(id_map, task_id);
        if (!target)
            continue;

        size_t dcount = cJSON_GetArraySize(deps_json);
        for (size_t j = 0; j < dcount; j++)
        {
            cJSON *dj = cJSON_GetArrayItem(deps_json, j);
            const char *src_id = cJSON_GetStringValue(cJSON_GetObjectItem(dj, "source_task_id"));
            struct edc_task *source = task_map_get(id_map, src_id);
            if (!source)
                continue;

            edc_task_dep_type dt = dep_type_from_name(cJSON_GetStringValue(cJSON_GetObjectItem(dj, "type")));
            edc_task_add_dependency(target, source, dt);

            cJSON *comp = cJSON_GetObjectItem(dj, "completed");
            cJSON *canc = cJSON_GetObjectItem(dj, "cancelled");
            cJSON *inj = cJSON_GetObjectItem(dj, "injected");

            if (comp && cJSON_IsTrue(comp))
                edc_task_on_dep_completed(target, source);
            if (canc && cJSON_IsTrue(canc))
                edc_task_on_dep_cancelled(target, source);
            if (inj && cJSON_IsTrue(inj))
            {
                struct edc_task_dep *dep;
                LL_FOREACH(target->deps, dep)
                if (dep->source == source)
                {
                    dep->injected = true;
                    break;
                }
            }
        }
    }
}

/* 依赖重建后的最终对账: 按 dep 标志 (completed/cancelled) 重算每个 task 的
 * deps_total / deps_completed, 并在所有依赖已满足时把 WAITING_DEPS 收敛到
 * WAITING_INPUT。运行时取消/完成事件按时间序触发, 恢复时 JSON 里的 deps
 * 顺序不定, 逐条 replay on_dep_cancelled/on_dep_completed 会产生与运行时
 * 不一致的中间状态, 这里一次性收敛, 保证确定性。 */
static void deser_reconcile_deps(struct task_map_entry **id_map)
{
    for (int b = 0; b < TASK_MAP_BUCKETS; b++)
    {
        struct task_map_entry *e = id_map[b];
        while (e)
        {
            struct edc_task *task = e->task;
            if (task)
            {
                size_t total = 0, completed = 0;
                struct edc_task_dep *dep;
                LL_FOREACH(task->deps, dep)
                {
                    if (dep->cancelled)
                        continue;
                    total++;
                    if (dep->completed)
                        completed++;
                }
                task->deps_total = total;
                task->deps_completed = completed;
                if (task->state == TASK_WAITING_DEPS && completed >= total)
                    edc_task_set_state(task, TASK_WAITING_INPUT);
            }
            e = e->next;
        }
    }
}

static void deser_turns(cJSON *json, cJSON *messages_array, struct task_map_entry **id_map)
{
    cJSON *tasks_json = cJSON_GetObjectItem(json, "tasks");
    if (!tasks_json || !cJSON_IsArray(tasks_json))
        return;

    size_t tcount = cJSON_GetArraySize(tasks_json);
    for (size_t i = 0; i < tcount; i++)
    {
        cJSON *tj = cJSON_GetArrayItem(tasks_json, i);
        const char *task_id = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "task_id"));
        struct edc_task *task = task_map_get(id_map, task_id);
        if (!task)
            continue;

        cJSON *turns_json = cJSON_GetObjectItem(tj, "turns");
        if (turns_json && cJSON_IsArray(turns_json))
        {
            size_t trc = cJSON_GetArraySize(turns_json);
            for (size_t j = 0; j < trc; j++)
            {
                struct edc_turn *turn = turn_from_json(cJSON_GetArrayItem(turns_json, j), messages_array);
                if (!turn)
                    continue;
                free(turn->task_id);
                turn->task_id = strdup(task->task_id);
                edc_task_add_turn(task, turn);
            }
        }

        cJSON *at_json = cJSON_GetObjectItem(tj, "active_turn");
        if (at_json)
        {
            struct edc_turn *turn = turn_from_json(at_json, messages_array);
            if (turn)
            {
                free(turn->task_id);
                turn->task_id = strdup(task->task_id);
                task->active_turn = turn;
            }
        }
    }
}

static void deser_global_ptrs(cJSON *sj, struct edc_session *session, struct edc_scheduler *sched,
                              struct task_map_entry **id_map, struct group_map_entry **group_map)
{
    if (!sj)
        return;
    const char *at_id = cJSON_GetStringValue(cJSON_GetObjectItem(sj, "active_task_id"));
    if (at_id)
        session->active_task = task_map_get(id_map, at_id);
    if (sched)
    {
        const char *fg_id = cJSON_GetStringValue(cJSON_GetObjectItem(sj, "foreground_group_id"));
        if (fg_id)
            sched->foreground_group = group_map_get(group_map, fg_id);
    }
}

bool edc_state_deserialize(cJSON *json, struct edc_session *session, struct edc_scheduler *sched)
{
    if (!json || !session)
        return false;

    cJSON *ver = cJSON_GetObjectItem(json, "version");
    if (!ver || !cJSON_IsNumber(ver) || cJSON_GetNumberValue(ver) != 1)
    {
        log_error("deserialize: unsupported or missing version");
        return false;
    }

    session->groups = NULL;
    session->active_task = NULL;

    cJSON *messages_array = session->messages;
    if (!messages_array)
    {
        log_error("deserialize: session->messages is NULL");
        return false;
    }

    cJSON *sj = cJSON_GetObjectItem(json, "session");
    deser_session_meta(session, sj);

    struct task_map_entry *id_map[TASK_MAP_BUCKETS] = {0};
    deser_create_tasks(json, id_map);
    deser_fork_chains(json, id_map);

    struct group_map_entry *group_map[GROUP_MAP_BUCKETS] = {0};
    deser_create_groups(json, session, id_map, group_map);
    deser_deps(json, id_map);
    deser_reconcile_deps(id_map);
    deser_turns(json, messages_array, id_map);
    deser_global_ptrs(sj, session, sched, id_map, group_map);
    deser_rebuild_group_history(session, sched);

    task_map_free(id_map);
    group_map_free(group_map);
    return true;
}
