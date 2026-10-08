#include "edc_state_serializer.h"
#include "edc_config.h"
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

/* ── On-disk schema version ─────────────────────────────────────────────
 * The loader accepts ONLY this exact number (see edc_state_deserialize): when
 * the shape changes, or a legacy shape is finally retired, bump it and ship a
 * one-shot migration under tools/ (e.g. migrate_session_v3_to_v4.py) instead of
 * teaching the load path to patch old data up on every load. */
#define EDC_STATE_SCHEMA_VERSION 5

/* ══════════════════════════════════════════════════════════════
 *  Internal helpers — dep type ↔ string
 *  ══════════════════════════════════════════════════════════════ */

static const char *dep_type_name(edc_task_dep_type t)
{
    switch (t)
    {
    case TASK_DEP_JOIN:
        return "JOIN";
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
    if (strcmp(name, "SEQUENCE") == 0)
        return TASK_DEP_SEQUENCE;
    return TASK_DEP_JOIN;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single turn
 *
 *  单一模式: 把消息体内联进 turn (消息归属 turn)。顺序由 task 的
 *  completion_seq + turn/消息内序决定 (见 deser_rebuild_group_history)。
 *  ══════════════════════════════════════════════════════════════ */

static cJSON *turn_to_json(struct edc_turn *turn)
{
    cJSON *tj = cJSON_CreateObject();

    cJSON *embedded = cJSON_CreateArray();
    for (size_t i = turn->messages.head; i < turn->messages.tail; i++)
    {
        cJSON *msg = (cJSON *)turn->messages.data[i];
        if (!msg)
            continue;

        cJSON *copy = cJSON_Duplicate(msg, 1);
        /* 消息是**整体往返的原始 JSON** (见 turn_from_json 的 cJSON_Duplicate),
         * 不象 task/group 那样逐字段重建 —— 未知键本来会一直跟着走。运行期
         * 不再替旧存档摘键: pre-transcript 的残留 (displayed/header、system
         * 容器) 由一次性 v3→v4 迁移清掉
         * (tools/migrate_session_v3_to_v4.py), loader 只收当前 schema。 */
        cJSON_AddItemToArray(embedded, copy);
    }

    if (cJSON_GetArraySize(embedded) > 0)
        cJSON_AddItemToObject(tj, "messages", embedded);
    else
        cJSON_Delete(embedded);

    return tj;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single task
 *  ══════════════════════════════════════════════════════════════ */

static cJSON *task_to_json(struct edc_task *task)
{
    cJSON *tj = cJSON_CreateObject();

    /* ── identity ── */
    cJSON_AddStringToObject(tj, "task_id", task->task_id ? task->task_id : "");
    cJSON_AddStringToObject(tj, "group_id", task->group_id ? task->group_id : "");
    cJSON_AddStringToObject(tj, "session_id", task->session_id ? task->session_id : "");

    /* ── description & state ── */
    cJSON_AddStringToObject(tj, "description", task->description ? task->description : "");
    /* 用户原文 (这个 task 是由哪条输入建成的)。**可选键**: 只有"由某条输入建成"
     * 的任务才有它 (fold 的 system task、fork 的孩子、单测直接建的都没有)。
     * 老文件没有这个键 → 读回 NULL, 所以不需要 schema 版本变更 (任务本来就是
     * 逐字段重建的, 未知键一律忽略)。 */
    if (task->raw_input)
        cJSON_AddStringToObject(tj, "raw_input", task->raw_input);
    /* v5: 存**两件正交的事**, 而不是把五个相位塌成一个常量。
     * step = 在哪一步 (原样存, 读回也原样), life = 活着吗。 */
    cJSON_AddStringToObject(tj, "step", edc_task_step_name(task->step));
    cJSON_AddStringToObject(tj, "life", edc_task_life_name(task->life));
    cJSON_AddBoolToObject(tj, "is_group_leader", task->is_group_leader);

    /* ── sink / fold target ──
     * A fold task (sink = GROUP_MEMORY) is now an ordinary persisted task, so its
     * output target and watermark must round-trip; otherwise the resumed fold
     * would not know which group to write. */
    cJSON_AddStringToObject(tj, "sink", edc_task_sink_name(task->sink));
    if (task->sink_group && task->sink_group->group_id)
        cJSON_AddStringToObject(tj, "sink_group_id", task->sink_group->group_id);
    cJSON_AddNumberToObject(tj, "fold_max_seq", (int)task->fold_max_seq);

    /* ── prompt ── */
    if (task->prompt_name)
        cJSON_AddStringToObject(tj, "prompt_name", task->prompt_name);
    if (task->prompt)
        cJSON_AddStringToObject(tj, "prompt", task->prompt);

    /* ── channel ── */
    cJSON_AddItemToObject(tj, "channel", edc_channel_to_json());

    /* ── fork chain ── */
    if (task->forked_from)
        cJSON_AddStringToObject(tj, "forked_from_task_id", task->forked_from->task_id);
    cJSON_AddNumberToObject(tj, "fork_point_completion_seq", (int)task->fork_point_completion_seq);

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

    /* ── token 统计 ──
     * 跨会话/进程中断保持: resume 后侧边栏的 per-task token 计数不归零。
     * cJSON 无 64 位整型, 用 double 承载 (计数远小于 2^53)。 */
    cJSON_AddNumberToObject(tj, "tokens_prompt", (double)task->tokens_prompt);
    cJSON_AddNumberToObject(tj, "tokens_completion", (double)task->tokens_completion);
    cJSON_AddNumberToObject(tj, "tokens_total", (double)task->tokens_total);
    cJSON_AddNumberToObject(tj, "tokens_cached", (double)task->tokens_cached);

    /* ── redirect ── */
    if (task->redirect_stdout)
        cJSON_AddStringToObject(tj, "redirect_stdout", task->redirect_stdout);
    if (task->redirect_stderr)
        cJSON_AddStringToObject(tj, "redirect_stderr", task->redirect_stderr);
    cJSON_AddBoolToObject(tj, "redirect_append", task->redirect_append);

    /* ── todos ── */
    if (task->todos)
        cJSON_AddItemToObject(tj, "todos", cJSON_Duplicate(task->todos, 1));
    /* todo 同步节流计数 (距上次 todowrite 的 LLM 请求数, 跨会话保持) */
    cJSON_AddNumberToObject(tj, "todo_sync_gap", task->todo_sync_gap);

    /* ── 未完成工具调用 (resume 账本) ── */
    if (task->tool_calls)
    {
        cJSON *tc = cJSON_CreateArray();
        struct edc_tool_call_record *rec;
        for (rec = task->tool_calls; rec; rec = rec->next)
        {
            cJSON *o = cJSON_CreateObject();
            if (rec->call_id)
                cJSON_AddStringToObject(o, "call_id", rec->call_id);
            if (rec->name)
                cJSON_AddStringToObject(o, "name", rec->name);
            if (rec->args)
                cJSON_AddItemToObject(o, "args", cJSON_Duplicate(rec->args, 1));
            cJSON_AddStringToObject(o, "status", rec->status == TOOL_CALL_EXECUTING ? "executing" : "pending");
            cJSON_AddItemToArray(tc, o);
        }
        cJSON_AddItemToObject(tj, "tool_calls", tc);
    }

    /* ── turns (committed) ── */
    cJSON *turns_json = cJSON_CreateArray();
    {
        size_t head = task->turns.head;
        size_t tail = task->turns.tail;
        for (size_t i = head; i < tail; i++)
        {
            struct edc_turn *t = (struct edc_turn *)task->turns.data[i];
            cJSON_AddItemToArray(turns_json, turn_to_json(t));
        }
    }
    cJSON_AddItemToObject(tj, "turns", turns_json);

    /* ── active_turn (在途回合, 独立字段) ── */
    if (task->active_turn)
        cJSON_AddItemToObject(tj, "active_turn", turn_to_json(task->active_turn));

    return tj;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single group
 *  ══════════════════════════════════════════════════════════════ */

/* ── 落盘判据: 谁上盘 ────────────────────────────────────────
 * lifetime 轴已退休: 每个任务都上盘, 不再有"不活过重启"的任务。于是这里只剩两条 ——
 * 一个组没有成员就不写盘 (不特判 kind), 以及下面 tasks 块里"空壳任务"那一关。 */

static cJSON *group_to_json(struct edc_task_group *group)
{
    cJSON *gj = cJSON_CreateObject();

    cJSON_AddStringToObject(gj, "group_id", group->group_id ? group->group_id : "");
    /* leader_task_id: for normal groups use leader->task_id;
     * for orphaned groups fall back to group_id (original leader's id). */
    if (group->leader)
        cJSON_AddStringToObject(gj, "leader_task_id", group->leader->task_id);
    else if (group->group_id)
        cJSON_AddStringToObject(gj, "leader_task_id", group->group_id);
    cJSON_AddBoolToObject(gj, "is_orphaned", group->is_orphaned);
    if (group->memory)
        cJSON_AddStringToObject(gj, "memory", group->memory);
    cJSON_AddNumberToObject(gj, "folded_through_completion_seq", (int)group->folded_through_completion_seq);

    /* member task ids */
    cJSON *members = cJSON_CreateArray();
    struct edc_task *m;
    LL_FOREACH2(group->members, m, group_next)
        cJSON_AddItemToArray(members, cJSON_CreateString(m->task_id ? m->task_id : ""));
    cJSON_AddItemToObject(gj, "member_task_ids", members);

    return gj;
}

/* ══════════════════════════════════════════════════════════════
 *  Public: serialize
 *  ══════════════════════════════════════════════════════════════ */

cJSON *edc_state_serialize(struct edc_session *session, struct edc_scheduler *sched)
{
    (void)sched; /* 前台组现在只存在 session 上, 序列化不再需要 scheduler */
    if (!session)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", EDC_STATE_SCHEMA_VERSION);

    /* ── session block ── */
    cJSON *sj = cJSON_CreateObject();
    cJSON_AddStringToObject(sj, "session_id", session->session_id ? session->session_id : "");
    cJSON_AddStringToObject(sj, "name", session->name ? session->name : "");
    cJSON_AddStringToObject(sj, "created_at", session->created_at ? session->created_at : "");
    cJSON_AddStringToObject(sj, "last_active", session->last_active ? session->last_active : "");
    if (session->workdir)
        cJSON_AddStringToObject(sj, "workdir", session->workdir);
    if (session->active_task)
        cJSON_AddStringToObject(sj, "active_task_id", session->active_task->task_id);
    /* 前台组是 session 自己的属性 —— 直接读它, 后台 session 落盘也带得回正确 id。
     * (scheduler 不再持有副本, 故无需回退。) */
    if (session->foreground_group)
        cJSON_AddStringToObject(sj, "foreground_group_id", session->foreground_group->group_id);
    cJSON_AddNumberToObject(sj, "next_completion_seq", (int)session->next_completion_seq);
    cJSON_AddItemToObject(root, "session", sj);

    /* ── groups block ── */
    cJSON *groups_json = cJSON_CreateArray();
    {
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
        {
            /* 空组不写盘 (不特判 kind)。 */
            if (g->member_count == 0)
                continue; /* skip empty */
            cJSON_AddItemToArray(groups_json, group_to_json(g));
        }
    }
    cJSON_AddItemToObject(root, "groups", groups_json);

    /* ── tasks block ── */
    cJSON *tasks_json = cJSON_CreateArray();
    {
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                /* 落盘判据 (retire-waiting-input.md §4 第 4 行):
                 *   丢 "READY 且身上什么都没有" 的任务, 不丢真任务。
                 *
                 * 为什么是这三个条件的合取:
                 *  - step==READY && life==ALIVE (== 旧 `state == TASK_IDLE`) 给出
                 *    "停着"的候选;
                 *  - 但 "停在 READY" 有两种截然不同的来源, 而 task 上没有字段能区分:
                 *    ① 旧模型残渣 —— 创建后不带 input 就停在那儿 (fork leader、
                 *       /task join 的 Joined task、@task 无正文), 它不该存在;
                 *    ② 被兜底/迁移强行放回 READY 的**真任务** (resume 的三条死胡同
                 *       从 TOOL_CALLING/LLM_PROCESSING 强制回 READY), 它这时
                 *       `active_turn` 和工具账本都还在, 丢掉就是数据丢失。
                 *    两个内容条件正是把 ② 择出去。
                 *
                 * 新模型下 ① 不该再被生产出来 (创建任务必然带着它的 input; 休眠态
                 * 走 SUSPENDED + /task resume) —— 如果真的落进这里, 说明还有调用方
                 * 在造 turn-less 任务, 于是告警而不是静默吞掉。 */
                if (t->step == EDC_TASKSTEP_READY && t->life == EDC_LIFE_ALIVE && !t->active_turn &&
                    !t->tool_calls)
                {
                    log_warn("serialize: dropping a task with no input: task=%s group=%s "
                             "(created without a turn — the retired 'waiting for input' shape)",
                             t->task_id ? t->task_id : "(null)", t->group_id ? t->group_id : "(null)");
                    continue;
                }
                cJSON_AddItemToArray(tasks_json, task_to_json(t));
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
 * Normalize a state read back from disk.
 *
 * 进程退出时所有未完成任务都被原样落盘 (WAITING_INPUT / WAITING_DEPS /
 * LLM_PROCESSING / TOOL_CALLING)。加载时一律收敛为 SUSPENDED —— 即"重新
 * 打开后先全部停住, 由用户对单个 task 发 resume 再重新判定该回到
 * WAITING_DEPS / WAITING_INPUT / LLM_PROCESSING 还是继续工具链"。
 * 终态 (COMPLETED / CANCELLED / ERROR) 与已 SUSPENDED 的原样保留。
 */
/**
 * Rebuild a turn from JSON: its embedded "messages" array.
 *
 * Returns a new edc_turn with messages populated.
 */
static struct edc_turn *turn_from_json(cJSON *tj)
{
    struct edc_turn *turn = edc_turn_new();

    cJSON *embedded = cJSON_GetObjectItem(tj, "messages");
    if (embedded && cJSON_IsArray(embedded))
    {
        size_t n = cJSON_GetArraySize(embedded);
        for (size_t i = 0; i < n; i++)
        {
            cJSON *msg = cJSON_GetArrayItem(embedded, i);
            cJSON *copy = cJSON_Duplicate(msg, 1);
            edc_queue_push(&turn->messages, copy);
        }
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
            sds prompt = edc_context_build_system_prompt(session->config, session->workdir);
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            cJSON_AddStringToObject(sys, "content", prompt);
            edc_task_group_history_add_owned(g, sys);
            sdsfree(prompt);
        }

        /* 按 completion_seq 升序重建 history，与运行时 (STOP/完成顺序) 保持一致。
         * 成员链表是创建顺序，若任务完成顺序与创建顺序不同，直接遍历会让
         * 折叠水位与切片错位，进而切断 assistant(tool_calls)+tool 序列。 */
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
                    edc_task_group_history_add_ref(g, turn->messages.data[mj]);
            }
        }
        free(arr);
    }

    if (sched)
    {
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
                edc_scheduler_observe_task_id(sched, t->task_id);
        }
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

    /* Restore the bound working directory. Tolerate a missing/deleted directory
     * (e.g. an unmounted path): keep the session unbound rather than failing the
     * whole load. edc_session_set_workdir re-grants it in the sandbox. */
    cJSON *wd_j = cJSON_GetObjectItem(sj, "workdir");
    if (wd_j && cJSON_IsString(wd_j) && strlen(cJSON_GetStringValue(wd_j)) > 0)
    {
        EdcError *werr = NULL;
        if (!edc_session_set_workdir(session, cJSON_GetStringValue(wd_j), &werr) && werr)
        {
            log_warn("session %s: cannot restore working directory '%s': %s", session->session_id,
                     cJSON_GetStringValue(wd_j), werr->message);
            edc_error_free(werr);
        }
    }
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

        /* 磁盘上的 "channel" 键是兼容物 (唯一的通道 = adapter): 读端忽略它的值。 */

        struct edc_task *task =
            edc_task_new(task_id, cJSON_GetStringValue(cJSON_GetObjectItem(tj, "session_id")) ?: "",
                         cJSON_GetStringValue(cJSON_GetObjectItem(tj, "group_id")) ?: "",
                         cJSON_GetStringValue(cJSON_GetObjectItem(tj, "description")) ?: "",
                         edc_channel_adapter);
        if (!task)
            continue;

        /* 用户原文 (可选键: 老文件没有它 → NULL, 与 fork/system task 一样)。 */
        {
            const char *raw_input = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "raw_input"));
            if (raw_input && *raw_input)
                task->raw_input = sdsnew(raw_input);
        }

        /* v5: step 与 life 分别读回 —— 相位**不再被粗化**。
         *
         * 唯一收敛的是 life: 重启之后没有任何东西在跑, 所以 ALIVE 落地成
         * SUSPENDED。但 **step 原样保留** —— 这正是这次改动要保住的东西: 以前
         * normalize_interrupted_state 把五个相位一起塌成 SUSPENDED, "任务到哪了"
         * 就此丢失, resume 只能靠工具账本 / active_turn 顺序探测把它猜回来。
         *
         * 例外: 折叠任务 (sink = GROUP_MEMORY)。它现在也落盘, 好让被重启打断的
         * 那一轮能续跑 —— 在途请求没了, 所以置回 READY, 由消息末尾的扫描重新派发
         * (它保留 active_turn / sink_group / fold_max_seq)。 */
        task->step = edc_task_classify_step(cJSON_GetStringValue(cJSON_GetObjectItem(tj, "step")));
        task->life = edc_task_classify_life(cJSON_GetStringValue(cJSON_GetObjectItem(tj, "life")));
        {
            cJSON *sv = cJSON_GetObjectItem(tj, "sink");
            if (sv && cJSON_IsString(sv))
                task->sink = edc_task_classify_sink(cJSON_GetStringValue(sv));
        }
        if (task->life == EDC_LIFE_ALIVE)
        {
            if (task->sink == EDC_SINK_GROUP_MEMORY)
                task->step = EDC_TASKSTEP_READY;
            else
                task->life = EDC_LIFE_SUSPENDED;
        }

        cJSON *v;
        if ((v = cJSON_GetObjectItem(tj, "is_group_leader")))
            task->is_group_leader = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "prompt_name")) && cJSON_IsString(v))
            task->prompt_name = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "prompt")) && cJSON_IsString(v))
            task->prompt = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "fork_point_completion_seq")) && cJSON_IsNumber(v))
            task->fork_point_completion_seq = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "output")) && cJSON_IsString(v))
            task->output = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "completion_seq")) && cJSON_IsNumber(v))
            task->completion_seq = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "fold_max_seq")) && cJSON_IsNumber(v))
            task->fold_max_seq = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "turns_folded")))
            task->turns_folded = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "tokens_prompt")) && cJSON_IsNumber(v))
            task->tokens_prompt = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "tokens_completion")) && cJSON_IsNumber(v))
            task->tokens_completion = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "tokens_total")) && cJSON_IsNumber(v))
            task->tokens_total = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "tokens_cached")) && cJSON_IsNumber(v))
            task->tokens_cached = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "redirect_stdout")) && cJSON_IsString(v))
            task->redirect_stdout = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "redirect_stderr")) && cJSON_IsString(v))
            task->redirect_stderr = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "redirect_append")))
            task->redirect_append = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "todos")) && cJSON_IsArray(v))
            task->todos = cJSON_Duplicate(v, 1);
        if ((v = cJSON_GetObjectItem(tj, "todo_sync_gap")) && cJSON_IsNumber(v))
            task->todo_sync_gap = (int)cJSON_GetNumberValue(v);

        /* ── 未完成工具调用 (resume 账本) ── */
        if ((v = cJSON_GetObjectItem(tj, "tool_calls")) && cJSON_IsArray(v))
        {
            size_t tcn = cJSON_GetArraySize(v);
            for (size_t k = 0; k < tcn; k++)
            {
                cJSON *o = cJSON_GetArrayItem(v, k);
                if (!o)
                    continue;
                const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(o, "call_id"));
                const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"));
                cJSON *targs = cJSON_GetObjectItem(o, "args");
                struct edc_tool_call_record *rec = edc_task_tool_call_add(task, cid, nm, targs);
                if (rec)
                {
                    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(o, "status"));
                    rec->status = (st && strcmp(st, "executing") == 0) ? TOOL_CALL_EXECUTING : TOOL_CALL_PENDING;
                }
            }
        }

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
        /* The system group is the one led by a fold task (sink = GROUP_MEMORY).
         * `kind` is derived from the leader's sink, not stored: the persistence
         * layer still knows nothing of "system group" as a concept, only of task
         * properties. */
        if (leader->sink == EDC_SINK_GROUP_MEMORY)
            group->kind = EDC_GROUP_KIND_SYSTEM;

        {
            cJSON *mem = cJSON_GetObjectItem(gj, "memory");
            if (mem && cJSON_IsString(mem))
                group->memory = sdsnew(cJSON_GetStringValue(mem));
        }
        {
            /* v3: 折叠水位 (completion_seq)。旧存档只有 fold_cursor(下标),
             * 由 deser_rebuild_group_history 依 turns_folded 推导, 这里不读。 */
            cJSON *fw = cJSON_GetObjectItem(gj, "folded_through_completion_seq");
            if (fw && cJSON_IsNumber(fw))
                group->folded_through_completion_seq = (uint32_t)cJSON_GetNumberValue(fw);
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

/* A fold task's sink_group is a live group pointer; the file stores its id. Resolve
 * it once the groups exist (same shape as deser_deps). */
static void deser_sink_groups(cJSON *json, struct task_map_entry **id_map, struct group_map_entry **group_map)
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
        const char *sg_id = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "sink_group_id"));
        if (!task_id || !sg_id || !*sg_id)
            continue;
        struct edc_task *task = task_map_get(id_map, task_id);
        if (!task)
            continue;
        task->sink_group = group_map_get(group_map, sg_id);
    }
}

static void deser_deps(cJSON *json, struct task_map_entry **id_map, struct edc_scheduler *sched)
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
            edc_task_dep_type dt = dep_type_from_name(cJSON_GetStringValue(cJSON_GetObjectItem(dj, "type")));
            struct edc_task *source = (src_id && src_id[0]) ? task_map_get(id_map, src_id) : NULL;
            if (!source)
            {
                /* Source not in this session → a cross-session edge (or a stale
                 * one). Record it for the app-level reconcile instead of
                 * silently dropping it: without the edge the target's
                 * deps_total would omit the dependency and it could be resumed
                 * before the source completes. */
                cJSON *comp0 = cJSON_GetObjectItem(dj, "completed");
                cJSON *canc0 = cJSON_GetObjectItem(dj, "cancelled");
                cJSON *inj0 = cJSON_GetObjectItem(dj, "injected");
                edc_scheduler_add_pending_dep(sched, task_id, src_id, dt, comp0 && cJSON_IsTrue(comp0),
                                              canc0 && cJSON_IsTrue(canc0), inj0 && cJSON_IsTrue(inj0));
                continue;
            }

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
                if (edc_task_awaits_deps(task) && completed >= total)
                    edc_task_mark_ready(task);
            }
            e = e->next;
        }
    }
}

static void deser_turns(cJSON *json, struct task_map_entry **id_map)
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
                struct edc_turn *turn = turn_from_json(cJSON_GetArrayItem(turns_json, j));
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
            struct edc_turn *turn = turn_from_json(at_json);
            if (turn)
            {
                free(turn->task_id);
                turn->task_id = strdup(task->task_id);
                task->active_turn = turn;
            }
        }
    }
}

static void deser_global_ptrs(cJSON *sj, struct edc_session *session,
                              struct task_map_entry **id_map, struct group_map_entry **group_map)
{
    if (!sj)
        return;
    const char *at_id = cJSON_GetStringValue(cJSON_GetObjectItem(sj, "active_task_id"));
    if (at_id)
        session->active_task = task_map_get(id_map, at_id);
    /* 前台组只记到属主 session —— 它本就是 session 的属性, scheduler 不持有
     * 副本 (装载 session 不再有副作用, 无需恢复当前视图)。 */
    const char *fg_id = cJSON_GetStringValue(cJSON_GetObjectItem(sj, "foreground_group_id"));
    if (fg_id)
        session->foreground_group = group_map_get(group_map, fg_id);
}

int edc_state_json_version(cJSON *json)
{
    if (!json)
        return 0;
    cJSON *ver = cJSON_GetObjectItem(json, "version");
    if (!ver || !cJSON_IsNumber(ver))
        return 0;
    return (int)cJSON_GetNumberValue(ver);
}

bool edc_state_deserialize(cJSON *json, struct edc_session *session, struct edc_scheduler *sched)
{
    if (!json || !session)
        return false;

    cJSON *ver = cJSON_GetObjectItem(json, "version");
    if (!ver || !cJSON_IsNumber(ver))
    {
        log_error("deserialize: unsupported or missing version");
        return false;
    }
    double ver_num = cJSON_GetNumberValue(ver);
    /* 只接受当前 schema。老版本必须先跑一次性迁移
     * (tools/migrate_session_v3_to_v4.py, 见 docs/session-model-v3.md §10) ——
     * 加载路径不再替旧数据做兼容修补。 */
    if (ver_num != EDC_STATE_SCHEMA_VERSION)
    {
        log_error("deserialize: unsupported version %g (expected %d)", ver_num, EDC_STATE_SCHEMA_VERSION);
        return false;
    }

    session->groups = NULL;
    session->active_task = NULL;

    cJSON *sj = cJSON_GetObjectItem(json, "session");
    deser_session_meta(session, sj);

    struct task_map_entry *id_map[TASK_MAP_BUCKETS] = {0};
    deser_create_tasks(json, id_map);
    deser_fork_chains(json, id_map);

    struct group_map_entry *group_map[GROUP_MAP_BUCKETS] = {0};
    deser_create_groups(json, session, id_map, group_map);
    deser_sink_groups(json, id_map, group_map);
    deser_deps(json, id_map, sched);
    deser_reconcile_deps(id_map);
    deser_turns(json, id_map);
    deser_global_ptrs(sj, session, id_map, group_map);
    deser_rebuild_group_history(session, sched);

    task_map_free(id_map);
    group_map_free(group_map);
    return true;
}
