#include "edc_state_serializer.h"
#include "edc_session_memory.h"
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
#define EDC_STATE_SCHEMA_VERSION 7

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
    case TASK_DEP_BARRIER:
        return "BARRIER";
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
    if (strcmp(name, "BARRIER") == 0)
        return TASK_DEP_BARRIER;
    return TASK_DEP_JOIN;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single turn
 *
 *  Single mode: inline the message bodies into the turn (messages belong to turns).
 *  Order is by the task's completion_seq + turn/message order (see
 *  deser_rebuild_group_history).
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
        /* A message round-trips as WHOLE raw JSON (see turn_from_json's
         * cJSON_Duplicate), not field-by-field like task/group -- unknown keys ride
         * along. The runtime no longer strips keys for old archives: pre-transcript
         * leftovers (displayed/header, the system container) are cleaned by the
         * one-shot v3->v4 migration (tools/migrate_session_v3_to_v4.py); the loader
         * only accepts the current schema. */
        cJSON_AddItemToArray(embedded, copy);
    }

    if (cJSON_GetArraySize(embedded) > 0)
        cJSON_AddItemToObject(tj, "messages", embedded);
    else
        cJSON_Delete(embedded);

    /* Messages already folded into group memory (the L1 fold cut): requests and history start after this. */
    cJSON_AddNumberToObject(tj, "folded_prefix", turn->folded_prefix);

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
    /* Raw user input this task was built from. An OPTIONAL key: only tasks built
     * from some input have it (fold system tasks, forked children and directly
     * built test tasks do not). Old files lack it -> reads back NULL, so no schema
     * bump is needed (tasks are rebuilt field-by-field and unknown keys are ignored). */
    if (task->raw_input)
        cJSON_AddStringToObject(tj, "raw_input", task->raw_input);
    /* v5: persist TWO orthogonal facts instead of collapsing five phases into one
     * constant. step = where (stored and read back verbatim), life = alive or not. */
    cJSON_AddStringToObject(tj, "step", edc_task_step_name(task->step));
    cJSON_AddStringToObject(tj, "life", edc_task_life_name(task->life));
    cJSON_AddBoolToObject(tj, "is_group_leader", task->is_group_leader);
    cJSON_AddBoolToObject(tj, "bg", task->is_background);
    /* `!cmd`: the task exists only to issue its seeded tool call. Optional key (absent =
     * false), so old files read back a normal task -- no schema bump. */
    if (task->tool_only)
        cJSON_AddBoolToObject(tj, "tool_only", true);
    /* Same shape: optional key, absent = an ordinary (curation) memory task. */
    if (task->memo_compact)
        cJSON_AddBoolToObject(tj, "memo_compact", true);

    /* ── sink / fold target ──
     * A fold task (sink = GROUP_MEMORY) is now an ordinary persisted task, so its
     * output target and cut must round-trip; otherwise the resumed fold would not
     * know which group to write, nor where this round's cut was (P2a-4). */
    cJSON_AddStringToObject(tj, "sink", edc_task_sink_name(task->sink));
    if (task->sink_group && task->sink_group->group_id)
        cJSON_AddStringToObject(tj, "sink_group_id", task->sink_group->group_id);
    if (task->fold_cut_task_id)
    {
        cJSON_AddStringToObject(tj, "fold_cut_task", task->fold_cut_task_id);
        cJSON_AddNumberToObject(tj, "fold_cut_turn", (int)task->fold_cut_turn);
        cJSON_AddNumberToObject(tj, "fold_cut_prefix", (int)task->fold_cut_prefix);
    }

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

    /* ── token accounting ──
     * Survives a session/process interruption: the sidebar's per-task token count
     * does not reset after resume. cJSON has no 64-bit int; double carries it. */
    cJSON_AddNumberToObject(tj, "tokens_prompt", (double)task->tokens_prompt);
    cJSON_AddNumberToObject(tj, "tokens_completion", (double)task->tokens_completion);
    cJSON_AddNumberToObject(tj, "tokens_total", (double)task->tokens_total);
    cJSON_AddNumberToObject(tj, "tokens_cached", (double)task->tokens_cached);
    cJSON_AddNumberToObject(tj, "last_total_tokens", (double)task->last_total_tokens);
    cJSON_AddNumberToObject(tj, "last_fold_epoch", (double)task->last_fold_epoch);
    cJSON_AddNumberToObject(tj, "last_hist_len", (double)task->last_hist_len);
    cJSON_AddNumberToObject(tj, "last_turn_len", (double)task->last_turn_len);

    /* L2 watermarks (v7): the session memory event table's "seen" position + the head
     * block's anchor tokens. Both travel with the task; the single writer is the task. */
    cJSON_AddNumberToObject(tj, "memo_seen_seq", (double)task->memo_seen_seq);
    cJSON_AddNumberToObject(tj, "last_prefix_tokens", (double)task->last_prefix_tokens);

    /* ── redirect ── */
    if (task->redirect_stdout)
        cJSON_AddStringToObject(tj, "redirect_stdout", task->redirect_stdout);
    if (task->redirect_stderr)
        cJSON_AddStringToObject(tj, "redirect_stderr", task->redirect_stderr);
    cJSON_AddBoolToObject(tj, "redirect_append", task->redirect_append);

    /* ── todos ── */
    if (task->todos)
        cJSON_AddItemToObject(tj, "todos", cJSON_Duplicate(task->todos, 1));
    /* todo sync throttle count (LLM requests since the last todowrite; survives sessions) */
    cJSON_AddNumberToObject(tj, "todo_sync_gap", task->todo_sync_gap);

    /* ── Incomplete tool calls (resume ledger) ── */
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

    /* ── active_turn (the in-flight turn, a separate field) ── */
    if (task->active_turn)
        cJSON_AddItemToObject(tj, "active_turn", turn_to_json(task->active_turn));

    return tj;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single group
 *  ══════════════════════════════════════════════════════════════ */

/* ── Persist predicate: who goes to disk ─────────────────────
 * The lifetime axis is retired: every task persists, and no task "does not survive a
 * restart". Two rules remain -- a group with no members is not written (no special
 * kind), and the "shell task" gate in the tasks block below. */

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
    cJSON_AddNumberToObject(gj, "fold_epoch", (int)group->fold_epoch);

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
    (void)sched; /* the foreground group lives on the session now; serialization no longer needs the scheduler */
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
    /* The foreground group is the session's own attribute -- read it directly; a
     * background session's persist brings back the right id (the scheduler keeps no
     * copy, so no fallback). */
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
            /* empty group: not written (no special kind) */
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
                /* Persist predicate: drop a task that is "READY with nothing on it",
                 * not a real task. The conjunction of three conditions:
                 *  - step==READY && life==ALIVE (== the old `state == TASK_IDLE`)
                 *    gives the "parked" candidate;
                 *  - but "parked at READY" has two very different sources and no task
                 *    field distinguishes them:
                 *    1) legacy residue -- created with no input and left there (fork
                 *       leaders, /task join's Joined task, @task with no body), which
                 *       should not exist;
                 *    2) a REAL task forced back to READY by fallback/migration (resume
                 *       forces the three dead ends back from TOOL_CALLING/
                 *       LLM_PROCESSING); its active_turn and tool ledger are still
                 *       there, so dropping it is data loss.
                 *    The two content conditions filter out (2).
                 * Under the new model (1) should no longer be produced (creating a task
                 * always carries its input; a stop uses INTERRUPTED + /task resume) -- if
                 * one still lands here, a caller is building turn-less tasks, so warn
                 * rather than swallow it. */
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

    /* ── L2: session-level memory (v7) ──
     * A top-level key (peer of session/groups/tasks): it is not a group's attribute.
     * Even an empty structure is written, so the loader need not handle a missing key. */
    cJSON_AddItemToObject(root, "session_memory", edc_session_memory_to_json(session->memo));

    return root;
}

/* ══════════════════════════════════════════════════════════════
 *  Deserialize helpers
 *  ══════════════════════════════════════════════════════════════ */

/**
 * Normalize a state read back from disk.
 *
 * On process exit edc_scheduler_close states the fact itself: every unfinished task is
 * written as INTERRUPTED ("the loop stopped underneath it" -- queue, in-flight, whatever),
 * and each one needs the user to decide. Load reads that back as stated: an INTERRUPTED
 * task stays INTERRUPTED until /task resume. The one fallback left is for a process that
 * died without a clean shutdown: an ALIVE task was in flight or queued when the loop
 * stopped, which is the same situation, so it is INTERRUPTED too. Terminal states
 * (COMPLETED / CANCELLED / ERROR) and memory-sink tasks are kept as-is.
 */
/**
 * Rebuild a turn from JSON: its embedded "messages" array.
 *
 * Returns a new edc_turn with messages populated.
 */
static struct edc_turn *turn_from_json(cJSON *tj)
{
    struct edc_turn *turn = edc_turn_new();

    cJSON *folded = cJSON_GetObjectItem(tj, "folded_prefix");
    if (folded && cJSON_IsNumber(folded))
        turn->folded_prefix = (int)cJSON_GetNumberValue(folded);

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
    /* One canonical rebuild (edc_task_group_rebuild_history): system prompt +
     * every member's committed turns in completion_seq order, minus each turn's
     * folded_prefix. The fold landing calls the same function, so a reloaded
     * history is byte-for-byte what the running process had. */
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
        edc_task_group_rebuild_history(g);

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

        /* The on-disk "channel" key is a compatibility artifact (the only channel = adapter); the reader ignores its value. */

        struct edc_task *task =
            edc_task_new(task_id, cJSON_GetStringValue(cJSON_GetObjectItem(tj, "session_id")) ?: "",
                         cJSON_GetStringValue(cJSON_GetObjectItem(tj, "group_id")) ?: "",
                         cJSON_GetStringValue(cJSON_GetObjectItem(tj, "description")) ?: "",
                         edc_channel_adapter);
        if (!task)
            continue;

        /* Raw user input (optional key: absent in old files -> NULL, like fork/system tasks). */
        {
            const char *raw_input = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "raw_input"));
            if (raw_input && *raw_input)
                task->raw_input = sdsnew(raw_input);
        }

        /* v5: step and life are read back separately -- the phase is NOT coarsened.
         *
         * Only life converges: after a restart nothing is running, so ALIVE lands as
         * SUSPENDED. But step is kept verbatim -- that is exactly what this change
         * preserves: normalize_interrupted_state used to collapse five phases into
         * SUSPENDED, losing "where the task was", and resume could only guess it back
         * from the tool ledger / active_turn.
         *
         * Exception: a fold task (sink = GROUP_MEMORY). It persists too now, so a
         * restart-interrupted round can resume -- the in-flight request is gone, so it
         * goes back to READY for the end-of-message scan to redispatch (it keeps its
         * active_turn / sink_group / cut triple). */
        task->step = edc_task_classify_step(cJSON_GetStringValue(cJSON_GetObjectItem(tj, "step")));
        task->life = edc_task_classify_life(cJSON_GetStringValue(cJSON_GetObjectItem(tj, "life")));
        {
            cJSON *sv = cJSON_GetObjectItem(tj, "sink");
            if (sv && cJSON_IsString(sv))
                task->sink = edc_task_classify_sink(cJSON_GetStringValue(sv));
        }
        if (task->life == EDC_LIFE_ALIVE)
        {
            if (edc_task_is_memory_sink(task))
                task->step = EDC_TASKSTEP_READY;
            else
                /* Nothing is running after a restart and nothing was decided about this
                 * task either. A clean shutdown states that itself before it saves
                 * (edc_scheduler_close -> edc_task_mark_interrupted); landing here means
                 * the process died without one, and the same state is what actually
                 * happened. No further inference: INTERRUPTED is read back as it is. */
                task->life = EDC_LIFE_INTERRUPTED;
        }

        cJSON *v;
        if ((v = cJSON_GetObjectItem(tj, "is_group_leader")))
            task->is_group_leader = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "bg")))
            task->is_background = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "tool_only")))
            task->tool_only = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "memo_compact")))
            task->memo_compact = cJSON_IsTrue(v);
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
        if ((v = cJSON_GetObjectItem(tj, "fold_cut_task")) && cJSON_IsString(v))
            task->fold_cut_task_id = strdup(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "fold_cut_turn")) && cJSON_IsNumber(v))
            task->fold_cut_turn = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "fold_cut_prefix")) && cJSON_IsNumber(v))
            task->fold_cut_prefix = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "tokens_prompt")) && cJSON_IsNumber(v))
            task->tokens_prompt = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "tokens_completion")) && cJSON_IsNumber(v))
            task->tokens_completion = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "tokens_total")) && cJSON_IsNumber(v))
            task->tokens_total = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "tokens_cached")) && cJSON_IsNumber(v))
            task->tokens_cached = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "last_total_tokens")) && cJSON_IsNumber(v))
            task->last_total_tokens = (long long)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "last_fold_epoch")) && cJSON_IsNumber(v))
            task->last_fold_epoch = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "last_hist_len")) && cJSON_IsNumber(v))
            task->last_hist_len = (int)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "last_turn_len")) && cJSON_IsNumber(v))
            task->last_turn_len = (int)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "memo_seen_seq")) && cJSON_IsNumber(v))
            task->memo_seen_seq = (uint64_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "last_prefix_tokens")) && cJSON_IsNumber(v))
            task->last_prefix_tokens = (uint64_t)cJSON_GetNumberValue(v);
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

        /* ── Incomplete tool calls (resume ledger) ── */
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

/* A task belongs to one group only: detach it from ALL of the session's groups (it
 * can only be on some group's member list). When disk shows "one task claimed by two
 * groups" (hand-edited / old archives / id collision), this prevents a double
 * membership, which would put the same object on two linked lists and double-free on
 * release (ASAN caught it). Scan the lists, not `task->group`: on the member path that
 * field is not necessarily authoritative.
 * only_group non-NULL = detach from that one group only (used when attaching a member
 * to its own group). */
static void deser_detach_from_groups(struct edc_session *session, struct edc_task *task, const char *task_id,
                                     struct edc_task_group *only_group)
{
    if (!session || !task)
        return;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        if (only_group && g != only_group)
            continue;
        /* Note LL_FOREACH2(..., group_next): the member chain runs on the task's
         * group_next field; the default `next` is a DIFFERENT chain, so scanning with
         * it does nothing (bitten before). Detach + break on match; the only mutation
         * happens before the break, so no safe variant is needed. */
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (t != task)
                continue;
            LL_DELETE2(g->members, t, group_next); /* the member chain's field is group_next, not next */
            if (g->member_count > 0)
                g->member_count--;
            log_warn("deserialize: task %s detached from group %s (a task belongs to one group only)",
                     task_id ?: "(null)", g->group_id ?: "(null)");
            break;
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
        /* A task belongs to one group (the member list is threaded by a SINGLE
         * group_next field). If disk shows it already on another group's member list
         * (hand-edited / old archives / id collision), detach first -- else the same
         * object would be on two lists and double-free on release (ASAN caught it). */
        deser_detach_from_groups(session, leader, leader_id, NULL);
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
            /* v3: fold watermark (completion_seq). Old archives only had fold_cursor
             * (an index), derived by deser_rebuild_group_history from turns_folded; not
             * read here. */
            /* Missing (old file) => 0: consistent with every task's last_fold_epoch,
             * and a missing position (last_hist_len==0) invalidates the anchor -- so an
             * old anchor is never trusted. */
            cJSON *fe = cJSON_GetObjectItem(gj, "fold_epoch");
            if (fe && cJSON_IsNumber(fe))
                group->fold_epoch = (uint32_t)cJSON_GetNumberValue(fe);
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
                if (!mt)
                    continue;
                /* Same: one group only -- detach from elsewhere before attaching. */
                deser_detach_from_groups(session, mt, mid, group);
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

/* Final reconciliation after dependency rebuild: recompute each task's deps_total /
 * deps_completed from the dep flags (completed/cancelled), and converge WAITING_DEPS
 * to WAITING_INPUT once all deps are satisfied. Runtime cancel/complete events fire in
 * time order, but JSON deps have no fixed order on restore, so replaying
 * on_dep_cancelled/on_dep_completed one by one would produce intermediate states
 * inconsistent with runtime; this converges once, for determinism. */
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
                /* Mirror the runtime cascade first: a dependency whose source ended
                 * without producing (ERROR / CANCELLED / INTERRUPTED) takes its dependent
                 * with it -- JOIN and SEQUENCE alike, exactly as
                 * edc_task_on_dep_failed/cancelled do at runtime, which the file may not
                 * have recorded (the source's end can be newer than the last save). An
                 * internal BARRIER is the exception: it is released instead. */
                struct edc_task_dep *dep;
                LL_FOREACH(task->deps, dep)
                {
                    if (dep->cancelled || dep->completed || !dep->source)
                        continue;
                    if (dep->source && edc_task_is_finished(dep->source) &&
                        !edc_task_is_completed(dep->source))
                    {
                        if (edc_task_is_cancelled(dep->source))
                            edc_task_on_dep_cancelled(task, dep->source);
                        else
                            /* ERROR and INTERRUPTED: the loop did not finish the source, and
                             * an interrupt is a manufactured error -- both cascade as a
                             * failure. */
                            edc_task_on_dep_failed(task, dep->source);
                    }
                }

                size_t total = 0, completed = 0;
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
    /* The foreground group is recorded only on its owning session -- it is a session
     * attribute and the scheduler holds no copy (loading a session has no side effect;
     * there is no current view to restore). */
    const char *fg_id = cJSON_GetStringValue(cJSON_GetObjectItem(sj, "foreground_group_id"));
    if (fg_id)
        session->foreground_group = group_map_get(group_map, fg_id);
}

int edc_state_schema_version(void)
{
    return EDC_STATE_SCHEMA_VERSION;
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
    /* Only the current schema is accepted. An old version must run the one-shot
     * migration first (tools/migrate_session_v3_to_v4.py, see
     * docs/session-model-v3.md §10) -- the load path no longer patches old data. */
    if (ver_num != EDC_STATE_SCHEMA_VERSION)
    {
        log_error("deserialize: unsupported version %g (expected %d)", ver_num, EDC_STATE_SCHEMA_VERSION);
        return false;
    }

    session->groups = NULL;
    session->active_task = NULL;

    /* L2 session memory (v7): a top-level key, read first -- every use is after this. */
    edc_session_memory_free(session->memo);
    session->memo = edc_session_memory_from_json(cJSON_GetObjectItem(json, "session_memory"));

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
