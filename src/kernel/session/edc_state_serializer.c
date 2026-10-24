#include "edc_state_serializer.h"
#include "edc_datetime_utils.h"
#include "edc_session_memory.h"
#include "edc_config.h"
#include "edc_compat.h"
#include "edc_containers.h"
#include "edc_context.h"
#include "edc_fold_files.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <string.h>

/* ── On-disk schema version ─────────────────────────────────────────────
 * The loader accepts any version in [MIN_SUPPORTED, CURRENT]. Older-but-supported
 * archives are read with defaults for the fields they lack, so a bump does not
 * strand them:
 *   - v7: no `turn->memo_prefix`, and `task->memo_compact` (bool) instead of `memo_kind`;
 *   - v8: no wall-clock on entries / events / group memory (reads back 0 = unknown);
 *   - v9: no `task->end` / `blocked_by` / `blocked_end` -- the reason a task ended is then
 *     inferred conservatively from `life` (CANCELLED -> STOPPED, ERROR -> FAILED), i.e.
 *     "not a cascade", so nothing is auto-released and nothing extra is counted as needing
 *     attention on the first load of an old session.
 *   - v10: no `task->fail` -- a failed task's reason is simply unknown, so a resume cannot
 *     warn "this will fail again".
 *   - v12: no `group->kind` -- the loader re-derived it from the leader sink, which
 *     lost "System" for a group led by an L2 curation task (the sidebar fell back to
 *     "Group <id>", and the next memory task then spawned yet another system group
 *     because no system group could be found).
 * Anything older must run its one-shot migration first
 * (tools/migrate_session_v*.py, see docs/session-model-v3.md §10). */
#define EDC_STATE_SCHEMA_VERSION 12
#define EDC_STATE_SCHEMA_MIN_SUPPORTED 7

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
    case TASK_DEP_UNLESS:
        return "UNLESS";
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
    if (strcmp(name, "UNLESS") == 0)
        return TASK_DEP_UNLESS;
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

    /* NO message array. Messages are rows (see edc_state_store.h, edc_msg_append): the
     * snapshot carries the turn's SHAPE -- how many messages it has and where its two
     * watermarks sit -- and that is all the metadata flush needs to write. Serializing
     * the bodies here is what used to make one tool boundary cost 164 MB. */
    cJSON_AddNumberToObject(tj, "msg_count", turn->msg_count);
    cJSON_AddNumberToObject(tj, "token_count", (double)edc_turn_tokens_from(turn, 0));
    /* The storage key travels with the turn: the position is data, not something the
     * reader can safely re-derive (an in-flight turn's ordinal is the queue tail, and the
     * rows are already written under it). */
    cJSON_AddNumberToObject(tj, "turn_ord", turn->turn_ord);
    if (turn->task_id > 0)
        cJSON_AddNumberToObject(tj, "owner_task_id", (double)turn->task_id);

    /* Messages already folded into group memory (the L1 fold cut): requests and history start after this. */
    cJSON_AddNumberToObject(tj, "folded_prefix", turn->folded_prefix);

    /* Messages already extracted into session memory (L2). A pure extraction
     * watermark: nothing reads it for request assembly. Invariant (repaired on read):
     * folded_prefix <= memo_prefix <= turn_len. */
    cJSON_AddNumberToObject(tj, "memo_prefix", turn->memo_prefix);

    return tj;
}

/* ══════════════════════════════════════════════════════════════
 *  Serialize a single task
 *  ══════════════════════════════════════════════════════════════ */

static cJSON *task_to_json(struct edc_task *task)
{
    cJSON *tj = cJSON_CreateObject();

    /* ── identity ── */
    cJSON_AddNumberToObject(tj, "task_id", (double)task->task_id);
    cJSON_AddNumberToObject(tj, "group_id", (double)task->group_id);
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
    /* v10: WHY it ended, which `life` alone cannot say (CANCELLED covers user stop,
     * `||` skip and dependency cascade; ERROR covers its own failure and a dependency
     * that never produced). Written even for NONE so an old binary reading a new file
     * still finds a consistent (step, life) pair. */
    cJSON_AddStringToObject(tj, "end", edc_task_end_name(task->end));
    if (task->end == EDC_END_BLOCKED)
    {
        if (task->blocked_by)
            cJSON_AddStringToObject(tj, "blocked_by", task->blocked_by);
        cJSON_AddStringToObject(tj, "blocked_end", edc_task_end_name(task->blocked_end));
    }
    /* v11: WHY it failed, when that is knowable. Written only for a classified failure --
     * an absent key means "not a failure or unknown", which is also what v10 archives say. */
    if (task->fail != EDC_FAIL_NONE)
        cJSON_AddStringToObject(tj, "fail", edc_task_fail_name(task->fail));
    cJSON_AddBoolToObject(tj, "is_group_leader", task->is_group_leader);
    cJSON_AddBoolToObject(tj, "bg", task->is_background);
    /* `!cmd`: the task exists only to issue its seeded tool call. Optional key (absent =
     * false), so old files read back a normal task -- no schema bump. */
    if (task->tool_only)
        cJSON_AddBoolToObject(tj, "tool_only", true);
    /* `{{…}}` references collected at input time but not expanded yet. Without this the
     * task comes back from disk with a LITERAL `{{LAST}}` in its message -- the expand
     * phase is gated on this flag (edc_llm_request), so a restart silently turned a task
     * reference into text. Optional key: absent = false, so no schema bump. */
    if (task->has_deferred_vars)
        cJSON_AddBoolToObject(tj, "deferred_vars", true);
    /* Which SESSION_MEMORY round this is: curation (default) / compaction / extraction.
     * Omitted for the default; a v7 archive carries the old `memo_compact` bool instead,
     * read back in the loader. */
    if (task->memo_kind != EDC_MEMO_KIND_CURATE)
        cJSON_AddStringToObject(tj, "memo_kind", edc_task_memo_kind_name(task->memo_kind));

    /* ── sink / fold target ──
     * A fold task (sink = GROUP_MEMORY) is now an ordinary persisted task, so its
     * output target and cut must round-trip; otherwise the resumed fold would not
     * know which group to write, nor where this round's cut was (P2a-4). */
    cJSON_AddStringToObject(tj, "sink", edc_task_sink_name(task->sink));
    if (task->sink_group && task->sink_group->group_id > 0)
        cJSON_AddNumberToObject(tj, "sink_group_id", (double)task->sink_group->group_id);
    if (task->fold_cut_task_id > 0)
    {
        cJSON_AddNumberToObject(tj, "fold_cut_task", (double)task->fold_cut_task_id);
        cJSON_AddNumberToObject(tj, "fold_cut_turn", (int)task->fold_cut_turn);
        cJSON_AddNumberToObject(tj, "fold_cut_prefix", (int)task->fold_cut_prefix);
    }
    /* Extraction rounds carry their own cut, same shape as the fold cut. */
    if (task->memo_cut_task_id > 0)
    {
        cJSON_AddNumberToObject(tj, "memo_cut_task", (double)task->memo_cut_task_id);
        cJSON_AddNumberToObject(tj, "memo_cut_turn", (int)task->memo_cut_turn);
        cJSON_AddNumberToObject(tj, "memo_cut_prefix", (int)task->memo_cut_prefix);
    }

    /* ── channel ── */
    cJSON_AddItemToObject(tj, "channel", edc_channel_to_json());

    /* ── fork chain ── */
    if (task->forked_from)
        cJSON_AddNumberToObject(tj, "forked_from_task_id", (double)task->forked_from->task_id);
    cJSON_AddNumberToObject(tj, "fork_point_completion_seq", (int)task->fork_point_completion_seq);

    /* ── deps (outgoing only) ── */
    cJSON *deps = cJSON_CreateArray();
    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "source_task_id", dep->source ? (double)dep->source->task_id : 0);
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
    cJSON_AddBoolToObject(tj, "redirect_stderr_append", task->redirect_stderr_append);

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

    cJSON_AddNumberToObject(gj, "group_id", (double)group->group_id);
    /* No "leader_task_id": the leader is the task whose id IS group_id (see
     * edc_task_group_new / set_leader). Writing it out would restate group_id, and the
     * old fallback branch -- group_id when the leader is gone -- proved it: both branches
     * produced the same string. */
    cJSON_AddBoolToObject(gj, "is_orphaned", group->is_orphaned);
    /* v12: the kind is a property of the group, decided when it is created (a task
     * adopted with EDC_TASK_PLACE_SYSTEM makes the system group), so it is written
     * out. Re-deriving it on load from the leader sink lost "System" for a group led
     * by an L2 curation task, and the next memory task then spawned yet another system
     * group because none could be found. */
    cJSON_AddStringToObject(gj, "kind", edc_task_group_kind_name(group->kind));
    if (group->memory)
        cJSON_AddStringToObject(gj, "memory", group->memory);
    /* The tracked-file lists are data, not text in `memory` (see edc_fold_files.h). Older
     * archives have no such key: they read back as an empty list, which is exactly right --
     * nothing migrates, because the next fold landing replaces `memory` wholesale. */
    if (group->fold_files && edc_fold_files_count(group->fold_files) > 0)
        cJSON_AddItemToObject(gj, "fold_files", edc_fold_files_to_json(group->fold_files));
    cJSON_AddNumberToObject(gj, "fold_epoch", (int)group->fold_epoch);
    /* Wall-clock about `memory` (v9). Optional keys: an old archive reads back 0 = unknown. */
    cJSON_AddNumberToObject(gj, "memory_created_at", (double)group->memory_created_at);
    cJSON_AddNumberToObject(gj, "memory_updated_at", (double)group->memory_updated_at);
    cJSON_AddNumberToObject(gj, "memory_covers_to", (double)group->memory_covers_to);

    /* member task ids */
    cJSON *members = cJSON_CreateArray();
    struct edc_task *m;
    LL_FOREACH2(group->members, m, group_next)
        cJSON_AddItemToArray(members, cJSON_CreateNumber((double)m->task_id));
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
    /* Integers, like every other timestamp in the store. This tree is internal: it is what
     * bind_session writes and what the deserializer reads back. */
    cJSON_AddNumberToObject(sj, "created_at", (double)session->created_at);
    cJSON_AddNumberToObject(sj, "last_active", (double)session->last_active);
    if (session->workdir)
        cJSON_AddStringToObject(sj, "workdir", session->workdir);
    if (session->active_task)
        cJSON_AddNumberToObject(sj, "active_task_id", (double)session->active_task->task_id);
    /* The foreground group is the session's own attribute -- read it directly; a
     * background session's persist brings back the right id (the scheduler keeps no
     * copy, so no fallback). */
    if (session->foreground_group)
        cJSON_AddNumberToObject(sj, "foreground_group_id", (double)session->foreground_group->group_id);
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
                 *       leaders, a legacy "Joined task", @task with no body), which
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
                    log_warn("serialize: dropping a task with no input: task=%lld group=%lld "
                             "(created without a turn — the retired 'waiting for input' shape)",
                             (long long)t->task_id, (long long)t->group_id);
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

    cJSON *memo = cJSON_GetObjectItem(tj, "memo_prefix");
    if (memo && cJSON_IsNumber(memo))
        turn->memo_prefix = (int)cJSON_GetNumberValue(memo);

    /* The bodies are NOT in the snapshot; they are rows, read on demand. Only the shape
     * comes back here. */
    cJSON *cnt = cJSON_GetObjectItem(tj, "msg_count");
    if (cnt && cJSON_IsNumber(cnt))
        turn->msg_count = (int)cJSON_GetNumberValue(cnt);

    cJSON *tord = cJSON_GetObjectItem(tj, "turn_ord");
    if (tord && cJSON_IsNumber(tord))
        turn->turn_ord = (int)cJSON_GetNumberValue(tord);

    /* Normalize: what is folded is in L2 by construction (the fold chain curated it),
     * and neither prefix may exceed the message count. A v7 archive lacks memo_prefix
     * entirely (reads back 0) => it becomes folded_prefix, which is exactly the old
     * binary's implicit assumption. */
    size_t len = (size_t)turn->msg_count;
    if (turn->memo_prefix < turn->folded_prefix)
        turn->memo_prefix = turn->folded_prefix;
    if (turn->memo_prefix > (int)len)
        turn->memo_prefix = (int)len;
    if (turn->folded_prefix > (int)len)
        turn->folded_prefix = (int)len;

    return turn;
}

/* ── Simple string→task hash map for deserialization ── */

struct task_map_entry
{
    int64_t task_id;
    struct edc_task *task;
    struct task_map_entry *next; /* singly-linked collision chain */
};

#define TASK_MAP_BUCKETS 64

/* Ids are numbers now, so the key is hashed directly. */
static unsigned int hash_id(int64_t id)
{
    unsigned long long x = (unsigned long long)id;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (unsigned int)x;
}

static void task_map_put(struct task_map_entry **map, int64_t id, struct edc_task *task)
{
    unsigned int b = hash_id(id) % TASK_MAP_BUCKETS;
    struct task_map_entry *e = g_new0(struct task_map_entry, 1);
    e->task_id = id;
    e->task = task;
    e->next = map[b];
    map[b] = e;
}

static struct edc_task *task_map_get(struct task_map_entry **map, int64_t id)
{
    if (id <= 0)
        return NULL;
    unsigned int b = hash_id(id) % TASK_MAP_BUCKETS;
    struct task_map_entry *e = map[b];
    while (e)
    {
        if (e->task_id == id)
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
            free(e);
            e = next;
        }
    }
}

/* An id out of a JSON object: a number (what this build writes) or a numeric string
 * (hand-written fixtures, and archives written before the ids became INTEGER). Anything
 * else -- a missing key, a non-numeric string -- reads as 0, "no id". */
static int64_t js_id(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem((cJSON *)o, key);
    if (!v)
        return 0;
    if (cJSON_IsNumber(v))
        return (int64_t)cJSON_GetNumberValue(v);
    if (cJSON_IsString(v))
    {
        const char *s = cJSON_GetStringValue((cJSON *)v);
        if (!s || !*s)
            return 0;
        char *end = NULL;
        const long long n = strtoll(s, &end, 10);
        if (end && *end == '\0' && n > 0)
            return (int64_t)n;
    }
    return 0;
}

/* ── Simple string→group hash map ── */

struct group_map_entry
{
    int64_t group_id;
    struct edc_task_group *group;
    struct group_map_entry *next;
};

#define GROUP_MAP_BUCKETS 16

static void group_map_put(struct group_map_entry **map, int64_t id, struct edc_task_group *group)
{
    unsigned int b = hash_id(id) % GROUP_MAP_BUCKETS;
    struct group_map_entry *e = g_new0(struct group_map_entry, 1);
    e->group_id = id;
    e->group = group;
    e->next = map[b];
    map[b] = e;
}

static struct edc_task_group *group_map_get(struct group_map_entry **map, int64_t id)
{
    if (id <= 0)
        return NULL;
    unsigned int b = hash_id(id) % GROUP_MAP_BUCKETS;
    struct group_map_entry *e = map[b];
    while (e)
    {
        if (e->group_id == id)
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
            /* Nothing to observe: the store assigns task ids, so there is no counter to
             * keep ahead of the loaded tree. */
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
    /* created_at: the session was constructed a moment ago, so without this the snapshot's
     * own creation time is replaced by "now" -- which is how an imported session ended up
     * dated the day it was imported instead of the day it was created. Both shapes are
     * accepted: an integer (current) and the display string older files carry. */
    cJSON *ca_j = cJSON_GetObjectItem(sj, "created_at");
    if (ca_j && cJSON_IsNumber(ca_j))
        session->created_at = (uint64_t)cJSON_GetNumberValue(ca_j);
    else if (ca_j && cJSON_IsString(ca_j))
    {
        time_t t = 0;
        if (edc_datetime_parse(cJSON_GetStringValue(ca_j), &t))
            session->created_at = (uint64_t)t;
    }
    cJSON *la_j = cJSON_GetObjectItem(sj, "last_active");
    if (la_j && cJSON_IsNumber(la_j))
        session->last_active = (uint64_t)cJSON_GetNumberValue(la_j);
    else if (la_j && cJSON_IsString(la_j))
    {
        /* A snapshot written before the timestamp cleanup. */
        time_t t = 0;
        if (edc_datetime_parse(cJSON_GetStringValue(la_j), &t))
            session->last_active = (uint64_t)t;
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
        const int64_t task_id = js_id(tj, "task_id");
        if (task_id <= 0)
            continue;

        /* The on-disk "channel" key is a compatibility artifact (the only channel = adapter); the reader ignores its value. */

        struct edc_task *task =
            edc_task_new(task_id, cJSON_GetStringValue(cJSON_GetObjectItem(tj, "session_id")) ?: "",
                         js_id(tj, "group_id"),
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
         * INTERRUPTED. But step is kept verbatim -- that is exactly what this change
         * preserves: normalize_interrupted_state used to collapse five phases into one
         * parked state, losing "where the task was", and resume could only guess it back
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

        /* v10: the reason it ended. A row saved while it was still ALIVE carries "NONE",
         * so an absent OR "NONE" reason falls back to life -- conservatively: a CANCELLED
         * one is treated as the user's stop and an ERROR one as its own failure, never as
         * a cascade (auto-releasing on a guess is exactly what this axis exists to avoid).
         * An ALIVE row that just became INTERRUPTED above gets INTERRUPTED too, so the
         * restart hint can count it. */
        {
            const char *end_name = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "end"));
            task->end = edc_task_classify_end(end_name);
            if (task->end == EDC_END_NONE)
            {
                if (task->life == EDC_LIFE_INTERRUPTED)
                    task->end = EDC_END_INTERRUPTED;
                else if (task->life == EDC_LIFE_CANCELLED)
                    task->end = EDC_END_STOPPED;
                else if (task->life == EDC_LIFE_ERROR)
                    task->end = EDC_END_FAILED;
            }

            if (task->end == EDC_END_BLOCKED)
            {
                const char *by = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "blocked_by"));
                if (by && *by)
                    task->blocked_by = strdup(by);
                const char *be = cJSON_GetStringValue(cJSON_GetObjectItem(tj, "blocked_end"));
                task->blocked_end = be ? edc_task_classify_end(be) : EDC_END_NONE;
            }

            /* v11: the failure class, if the archive recorded one (absent -> unknown). */
            task->fail = edc_task_classify_fail(cJSON_GetStringValue(cJSON_GetObjectItem(tj, "fail")));
        }

        cJSON *v;
        if ((v = cJSON_GetObjectItem(tj, "is_group_leader")))
            task->is_group_leader = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "bg")))
            task->is_background = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "tool_only")))
            task->tool_only = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(tj, "deferred_vars")))
            task->has_deferred_vars = cJSON_IsTrue(v) ? 1 : 0;
        /* v8: the tri-state tag. A v7 archive has no `memo_kind`; its old
         * `memo_compact` bool says COMPACT when true, and the default (CURATE) when
         * absent -- which is what memo_kind already holds. */
        if ((v = cJSON_GetObjectItem(tj, "memo_kind")) && cJSON_IsString(v))
            task->memo_kind = edc_task_classify_memo_kind(cJSON_GetStringValue(v));
        else if ((v = cJSON_GetObjectItem(tj, "memo_compact")) && cJSON_IsTrue(v))
            task->memo_kind = EDC_MEMO_KIND_COMPACT;
        if ((v = cJSON_GetObjectItem(tj, "fork_point_completion_seq")) && cJSON_IsNumber(v))
            task->fork_point_completion_seq = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "output")) && cJSON_IsString(v))
            task->output = sdsnew(cJSON_GetStringValue(v));
        if ((v = cJSON_GetObjectItem(tj, "completion_seq")) && cJSON_IsNumber(v))
            task->completion_seq = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "fold_cut_task")) && cJSON_IsNumber(v))
            task->fold_cut_task_id = (int64_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "fold_cut_turn")) && cJSON_IsNumber(v))
            task->fold_cut_turn = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "fold_cut_prefix")) && cJSON_IsNumber(v))
            task->fold_cut_prefix = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "memo_cut_task")) && cJSON_IsNumber(v))
            task->memo_cut_task_id = (int64_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "memo_cut_turn")) && cJSON_IsNumber(v))
            task->memo_cut_turn = (uint32_t)cJSON_GetNumberValue(v);
        if ((v = cJSON_GetObjectItem(tj, "memo_cut_prefix")) && cJSON_IsNumber(v))
            task->memo_cut_prefix = (uint32_t)cJSON_GetNumberValue(v);
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
        if ((v = cJSON_GetObjectItem(tj, "redirect_stderr_append")))
            task->redirect_stderr_append = cJSON_IsTrue(v);
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
        const int64_t tid = js_id(tj, "task_id");
        const int64_t fp = js_id(tj, "forked_from_task_id");
        if (tid > 0 && fp > 0)
        {
            struct edc_task *task = task_map_get(id_map, tid);
            struct edc_task *parent = task_map_get(id_map, fp);
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
static void deser_detach_from_groups(struct edc_session *session, struct edc_task *task, int64_t task_id,
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
            log_warn("deserialize: task %lld detached from group %lld (a task belongs to one group only)",
                     (long long)task_id, (long long)g->group_id);
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
        const int64_t group_id = js_id(gj, "group_id");
        if (group_id <= 0)
            continue;
        /* The leader IS the task whose id equals group_id -- there is no separate
         * leader_task_id field any more. `is_orphaned` says whether that task is still
         * the leader: an orphaned group kept the id but lost the leader. */
        const int64_t leader_id = group_id;
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
            {
                /* The first member's id: the array holds ids, so read element 0 (and accept
                 * a numeric string there too, like every other id). */
                const cJSON *m0 = cJSON_GetArrayItem(members, 0);
                int64_t mid0 = 0;
                if (cJSON_IsNumber(m0))
                    mid0 = (int64_t)cJSON_GetNumberValue((cJSON *)m0);
                else if (cJSON_IsString(m0))
                    mid0 = (int64_t)strtoll(cJSON_GetStringValue((cJSON *)m0), NULL, 10);
                if (mid0 > 0)
                    leader = task_map_get(id_map, mid0);
            }
        }
        if (!leader)
        {
            log_warn("deserialize: group %lld leader %lld not found",
                     (long long)group_id, (long long)leader_id);
            continue;
        }

        struct edc_task_group *group = edc_task_group_new(leader);
        if (!group)
            continue;
        /* The STORED group_id is the group's identity, and the leader is derived from it
         * (the leader is the task whose id IS group_id). edc_task_group_new() derives the
         * id from whatever task it was handed, which is only correct when that task is the
         * leader -- for an orphaned group it is not: the ex-leader belongs to no group any
         * more, so it is not serialized, so `leader` above fell back to a member and the
         * id would silently change to that member's task id. Every message row keyed on
         * the old id would then point at a group that no longer answers to it. */
        if (group_id > 0 && group->group_id != group_id)
        {
            group->group_id = group_id;
            /* Members carry their group's id too (denormalized); keep them in step. */
            struct edc_task *m;
            LL_FOREACH2(group->members, m, group_next)
                m->group_id = group_id;
        }
        if (is_orph)
            group->is_orphaned = true;
        /* The kind travels with the group (v12): only its creator knows it, and an L2
         * curation task leads a system group exactly like an L1 fold task does. There is
         * deliberately no fallback inferring it from the leader sink: an archive written
         * before v12 simply has no system group until one is created again. */
        const char *kind_name = cJSON_GetStringValue(cJSON_GetObjectItem(gj, "kind"));
        if (kind_name)
            group->kind = edc_task_group_classify_kind(kind_name);
        else
            log_debug("deserialize: group %lld carries no kind (pre-v12 archive) -- user group",
                      (long long)group_id);

        {
            cJSON *mem = cJSON_GetObjectItem(gj, "memory");
            if (mem && cJSON_IsString(mem))
                group->memory = sdsnew(cJSON_GetStringValue(mem));
        }
        {
            /* The tracked files (see edc_fold_files.h). A pre-existing archive has no key:
             * empty list, no migration -- the fold landing replaces `memory` wholesale. */
            cJSON *ff = cJSON_GetObjectItem(gj, "fold_files");
            if (ff)
                group->fold_files = edc_fold_files_from_json(ff);
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
        {
            /* v9: wall-clock about `memory`. Missing (old file) => 0 = unknown. */
            cJSON *v;
            if ((v = cJSON_GetObjectItem(gj, "memory_created_at")) && cJSON_IsNumber(v))
                group->memory_created_at = (uint64_t)cJSON_GetNumberValue(v);
            if ((v = cJSON_GetObjectItem(gj, "memory_updated_at")) && cJSON_IsNumber(v))
                group->memory_updated_at = (uint64_t)cJSON_GetNumberValue(v);
            if ((v = cJSON_GetObjectItem(gj, "memory_covers_to")) && cJSON_IsNumber(v))
                group->memory_covers_to = (uint64_t)cJSON_GetNumberValue(v);
        }

        cJSON *members = cJSON_GetObjectItem(gj, "member_task_ids");
        if (members && cJSON_IsArray(members))
        {
            size_t mcount = cJSON_GetArraySize(members);
            for (size_t j = 0; j < mcount; j++)
            {
                const cJSON *mj = cJSON_GetArrayItem(members, j);
                const int64_t mid = cJSON_IsNumber(mj) ? (int64_t)cJSON_GetNumberValue((cJSON *)mj) : 0;
                if (mid <= 0 || mid == leader->task_id)
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
        struct edc_task *task = task_map_get(id_map, js_id(tj, "task_id"));
        const int64_t sg = js_id(tj, "sink_group_id");
        if (!task || sg <= 0)
            continue;
        task->sink_group = group_map_get(group_map, sg);
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
        const int64_t tid = js_id(tj, "task_id");
        cJSON *deps_json = cJSON_GetObjectItem(tj, "deps");
        if (tid <= 0 || !deps_json || !cJSON_IsArray(deps_json))
            continue;

        struct edc_task *target = task_map_get(id_map, tid);
        if (!target)
            continue;

        size_t dcount = cJSON_GetArraySize(deps_json);
        for (size_t j = 0; j < dcount; j++)
        {
            cJSON *dj = cJSON_GetArrayItem(deps_json, j);
            const int64_t src_id = js_id(dj, "source_task_id");
            edc_task_dep_type dt = dep_type_from_name(cJSON_GetStringValue(cJSON_GetObjectItem(dj, "type")));
            struct edc_task *source = task_map_get(id_map, src_id);
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
                edc_scheduler_add_pending_dep(sched, tid, src_id, dt,
                                              comp0 && cJSON_IsTrue(comp0),
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
                 * have recorded (the source's end can be newer than the last save). The two
                 * RELEASING kinds are the exception: BARRIER (the order is satisfied) and
                 * UNLESS (`||` -- a source that did not succeed is why the dependent exists)
                 * both release instead. */
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
                    else if (dep->source && edc_task_is_completed(dep->source) &&
                             dep->type == TASK_DEP_UNLESS)
                    {
                        /* The inverse rule needs the same mirror: the source DID produce, so
                         * a `||` dependent must be skipped even though the file still says
                         * the edge is outstanding. */
                        edc_task_on_dep_completed(task, dep->source);
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
        struct edc_task *task = task_map_get(id_map, js_id(tj, "task_id"));
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
                /* The ordinal in the snapshot IS the storage key the rows were written
                 * under; falling back to the queue index keeps old snapshots readable. */
                const int ord = turn->turn_ord >= 0 ? turn->turn_ord : (int)edc_queue_length(&task->turns);
                const int64_t owner = js_id(cJSON_GetArrayItem(turns_json, j), "owner_task_id");
                const int64_t owner_key = owner > 0 ? owner : task->task_id;
                edc_turn_set_key(turn, task->session_id, task->group_id, owner_key, ord);
                edc_task_add_turn(task, turn);
            }
        }

        cJSON *at_json = cJSON_GetObjectItem(tj, "active_turn");
        if (at_json)
        {
            struct edc_turn *turn = turn_from_json(at_json);
            if (turn)
            {
                /* Keep the key the turn was stored under (see turn_to_json): the rows are
                 * already written against it. */
                const int64_t owner = js_id(at_json, "owner_task_id");
                const int64_t owner_key = owner > 0 ? owner : task->task_id;
                edc_turn_set_key(turn, task->session_id, task->group_id, owner_key, turn->turn_ord);
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
    session->active_task = task_map_get(id_map, js_id(sj, "active_task_id"));
    /* The foreground group is recorded only on its owning session -- it is a session
     * attribute and the scheduler holds no copy (loading a session has no side effect;
     * there is no current view to restore). */
    session->foreground_group = group_map_get(group_map, js_id(sj, "foreground_group_id"));
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
    /* Accept the current schema and the one right before it (see the schema-version
     * note at the top): a bump must not strand the archives the previous binary wrote.
     * Fields the older shape lacks read back as defaults. Older than that must run its
     * one-shot migration first -- the load path does not patch ancient data up. */
    if (ver_num < EDC_STATE_SCHEMA_MIN_SUPPORTED || ver_num > EDC_STATE_SCHEMA_VERSION)
    {
        log_error("deserialize: unsupported version %g (expected %d..%d)", ver_num, EDC_STATE_SCHEMA_MIN_SUPPORTED,
                  EDC_STATE_SCHEMA_VERSION);
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
