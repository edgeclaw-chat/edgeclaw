#include "edc_scheduler.h"
#include "edc_cmd_parse.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_group_fold.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_session_manager.h"
#include "edc_session_store.h"
#include "edc_service.h"
#include "edc_turn.h"
#include "log.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── uthash wrapper: request_id → edc_task ──────────────────── */

struct edc_sched_task_entry
{
    void *request_id;
    struct edc_task *task;
    UT_hash_handle hh;
};

/* ── Forward declarations ──────────────────────────────────── */

static void edc_scheduler_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg);
static void sched_save_session(struct edc_scheduler *sched, struct edc_session *s);
/* Sidebar-only task-tree snapshot, pulled by explicit session_id; the chat pane is the TUI's own. */
static void sched_handle_pull_snapshot(struct edc_scheduler *sched, const char *session_id);
static void sched_handle_task_snapshot(struct edc_scheduler *sched, struct edc_session *session, const char *reason);
static void sched_trace_notify(const char *reason, const char *session_id);
static void sched_force_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step,
                                  edc_task_life life);
/* The executor's outbound ops table (defined later in the file; forward-declared for init in new). */
static const struct edc_executor_ops sched_executor_ops;

/* Whether a member of a group still holds the front, i.e. whether a task that joins
 * behind it has to wait. Only work that can actually run counts: a task with no input
 * at all is never runnable, so it must not block the ones behind it forever. */
static bool sched_task_holds_front(const struct edc_task *task)
{
    return task && !edc_task_is_finished(task) && (task->active_turn || edc_task_is_running(task));
}

/* Put |task| at the back of the queue of the group it is in, when that group is the one
 * in front: one edge to the member that currently holds the front. Two doors lead to "a
 * task is about to run in the foreground group" -- creation (sched_settle_task) and
 * resume -- and both must respect the queue, or the group runs two at a time.
 * Returns true when an edge was added (false when there is nothing to wait for). */
static bool sched_chain_behind_front(struct edc_task *task)
{
    struct edc_session *own = edc_task_session(task);
    struct edc_task *prev = NULL;
    struct edc_task *m;

    if (!own || !task->group || own->foreground_group != task->group)
        return false;

    LL_FOREACH2(task->group->members, m, group_next)
    {
        /* Skip this task: it has already joined the group. */
        if (m != task && sched_task_holds_front(m))
            prev = m;
    }
    if (!prev)
        return false;

    return edc_task_add_dependency(task, prev, TASK_DEP_SEQUENCE);
}

/* The task's owner session -- now edc_task_session() in edc_task.c. */

/* The foreground group is a session attribute; the scheduler holds no copy. */
static struct edc_task_group *sched_foreground(struct edc_session *session)
{
    return session ? session->foreground_group : NULL;
}

/* Inbound session label -> session object; NULL makes the caller reject the input. */
static struct edc_session *sched_resolve_session(struct edc_scheduler *sched, const char *session_id)
{
    if (!sched || !sched->sessions)
        return NULL;
    return edc_session_store_resolve_session(sched->sessions, session_id);
}

/* "Some tree changed": which sessions changed, deduped by pointer, flushed at message end. */
static void sched_mark_dirty(struct edc_scheduler *sched, struct edc_session *session, const char *reason)
{
    if (!sched || !session)
        return;
    for (size_t i = 0; i < sched->dirty_count; i++)
    {
        if (sched->dirty[i].session == session)
            return; /* already marked dirty in this message */
    }
    if (sched->dirty_count == sched->dirty_cap)
    {
        size_t cap = sched->dirty_cap ? sched->dirty_cap * 2 : 4;
        struct edc_sched_dirty *grown = realloc(sched->dirty, cap * sizeof(*grown));
        if (!grown)
            return;
        sched->dirty = grown;
        sched->dirty_cap = cap;
    }
    sched->dirty[sched->dirty_count].session = session;
    sched->dirty[sched->dirty_count].reason = reason;
    sched->dirty_count++;
}

/* Settle this message's consequences: notify each dirty session, then clear. */
static void sched_drain_dirty(struct edc_scheduler *sched)
{
    if (!sched || sched->dirty_count == 0)
        return;
    for (size_t i = 0; i < sched->dirty_count; i++)
    {
        struct edc_session *session = sched->dirty[i].session;
        const char *reason = sched->dirty[i].reason;
        const char *sid = (session && session->session_id) ? session->session_id : NULL;
        sched_trace_notify(reason, sid);
        if (sched->snapshot_changed && sid)
            sched->snapshot_changed(sched, sid, reason);
    }
    sched->dirty_count = 0;
}

/* next_task_id is a process-wide monotonic allocator; scanning live tasks cannot
 * recover it, so the high-water mark is persisted in a tiny file under the
 * workspace. A write failure degrades to memory-only monotonicity. */

static sds sched_task_seq_path(struct edc_scheduler *sched)
{
    if (!sched || !sched->config || !sched->config->workspace || sdslen(sched->config->workspace) == 0)
        return NULL;
    return edc_path_join(sched->config->workspace, ".task_seq", NULL);
}

static void sched_persist_task_seq(struct edc_scheduler *sched)
{
    sds path = sched_task_seq_path(sched);
    if (!path)
        return;

    sds tmp = sdscatprintf(sdsempty(), "%s.tmp", path);
    /* Plain stdio, not edc_file_*: scheduler tests do not set up the uv file loop. */
    FILE *f = fopen(tmp, "w");
    if (!f && errno == ENOENT && sched->config && sched->config->workspace)
    {
        /* The workspace dir may not exist yet; create it and retry once. */
        mkdir(sched->config->workspace, 0755);
        f = fopen(tmp, "w");
    }
    if (!f)
    {
        log_debug("task_seq: open %s for write failed: %s", tmp, strerror(errno));
    }
    else
    {
        if (fprintf(f, "%lld\n", (long long)sched->next_task_id) < 0)
            log_debug("task_seq: write %s failed: %s", tmp, strerror(errno));
        fclose(f);
        if (rename(tmp, path) != 0)
            log_warn("task_seq: rename %s -> %s failed: %s", tmp, path, strerror(errno));
    }

    sdsfree(tmp);
    sdsfree(path);
}

static void sched_load_task_seq(struct edc_scheduler *sched)
{
    sds path = sched_task_seq_path(sched);
    if (!path)
        return;

    FILE *f = fopen(path, "r");
    sdsfree(path);
    if (!f)
        return; /* first run / missing file: keep the in-memory initial value */

    long long v = 0;
    if (fscanf(f, "%lld", &v) == 1 && v > sched->next_task_id)
        sched->next_task_id = v;
    fclose(f);
}

void edc_scheduler_observe_task_id(struct edc_scheduler *sched, const char *task_id)
{
    if (!sched || !task_id || !*task_id)
        return;

    /* Only plain decimal numbers; "task-1"-style ids in old archives/tests read as 0. */
    char *end = NULL;
    long long id = strtoll(task_id, &end, 10);
    if (end == task_id || (end && *end != '\0'))
        return;

    if (id >= sched->next_task_id)
    {
        sched->next_task_id = id + 1;
        sched_persist_task_seq(sched);
    }
}

/* ── Lifetime ──────────────────────────────────────────────── */

struct edc_scheduler *edc_scheduler_new(struct edc_config *config)
{
    struct edc_scheduler *sched = g_new0(struct edc_scheduler, 1);
    sched->config = config;
    sched->task_hash = NULL;
    sched->next_task_id = 1;

    /* Restore the previous run's high-water mark so task_id never regresses or repeats. */
    sched_load_task_seq(sched);

    edc_executor_init(&sched->executor, &sched_executor_ops, sched, config);

    sched->self = edc_actor_new("edc_scheduler", edc_scheduler_actor_handler, sched);

    return sched;
}

bool edc_scheduler_open(struct edc_scheduler *sched)
{
    if (!sched || sched->sessions)
        return false;

    struct edc_session_manager *sm = edc_session_manager_new(sched->config);
    if (!sm)
    {
        log_error("scheduler open: failed to create the session manager");
        return false;
    }
    sched->sessions = sm;
    sched->sessions_owned = true;
    sched->sessions_closed = false;

    /* Restore the saved task/group state of the active session. Safe here:
     * `sched` is fully constructed, and the load callbacks it triggers
     * (observe_task_id / add_pending_dep) target this same object. */
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (active)
        edc_session_store_load_session(sm, sched, active);
    return true;
}

/* Built-in service: look for an idle group with unfolded history and start one
 * extraction round. It only "knocks" -- edc_session_memory_extract_try decides (idle,
 * nothing in flight, no fold due) and is a cheap no-op when nothing is due, so a short
 * interval costs nothing but a function call. */
#define EDC_MEMO_EXTRACT_SCAN_MS (60 * 1000)

static void sched_service_extract_scan(struct edc_service *svc, void *ud)
{
    (void)svc;
    struct edc_scheduler *sched = ud;
    if (!sched || !sched->sessions)
        return;
    struct edc_session *got = NULL;
    if (edc_session_memory_extract_try(sched, sched->sessions, sched->config, &got) && got)
        sched_save_session(sched, got);
}

void edc_scheduler_services_start(struct edc_scheduler *sched)
{
    if (!sched)
        return;
    edc_service_register(sched, "extract-scan", EDC_MEMO_EXTRACT_SCAN_MS, sched_service_extract_scan, sched);
}

/* Shutdown pass: nothing will advance any more, so every task that is not already
 * terminal says so in its own state -- READY and waiting ones included: the queue they
 * were sitting in is over, and each of them needs a decision now. Memory-sink tasks
 * (fold / curation) are the exception: the scan re-dispatches a half-done fold on load,
 * which is what a background round wants. */
static void sched_interrupt_all(void *ctx, struct edc_task *task)
{
    (void)ctx;
    if (!task || edc_task_is_finished(task) || edc_task_is_memory_sink(task))
        return;

    edc_task_mark_interrupted(task);
    /* The broadcast moved dependents as a side effect and nobody will settle those
     * reports now. The flags they carried are what gets persisted, so dropping the list
     * only releases the bookkeeping (its owner is still alive at this point). */
    edc_task_drain_moves(task, NULL, NULL);
}

bool edc_scheduler_close(struct edc_scheduler *sched)
{
    if (!sched || !sched->sessions || sched->sessions_closed)
        return false;

    /* Exit = closing the process, not a user-driven /session remove: persist what
     * happened. Nothing will advance any more, so every unfinished task is stated as
     * INTERRUPTED first (tool ledger intact), and the user decides whether to resume one
     * by one -- hence no CANCELLED and no second save after cancelling. The interrupt
     * also broadcasts, so a task queued behind another one is not left waiting for a
     * predecessor that will never run on its own. */
    edc_session_manager_foreach_task(sched->sessions, sched_interrupt_all, NULL);
    edc_session_store_save_all_sessions_state(sched->sessions, sched);

    /* Nothing is in flight any more: forget the pending requests (a late reply must not
     * land) without touching the states just written. */
    edc_session_store_drop_in_flight_requests(sched->sessions, sched);

    /* Stop the background services' registry. Their single timer is closed by the
     * actor runner's shutdown walk (this runs after the runner is gone). */
    edc_services_close(sched);

    sched->sessions_closed = true;
    return true;
}

void edc_scheduler_free(struct edc_scheduler *sched)
{
    if (!sched)
        return;

    struct edc_sched_task_entry *entry, *tmp;
    HASH_ITER(hh, sched->task_hash, entry, tmp)
    {
        HASH_DEL(sched->task_hash, entry);
        free(entry);
    }

    /* Idempotent with edc_scheduler_close; frees the registry only (no uv: the timer is
     * closed by the actor runner). */
    edc_services_close(sched);

    /* Groups and their tasks are owned by their session (freed in edc_session_free). */

    struct edc_pending_dep *pd, *pd_tmp;
    for (pd = sched->pending_deps; pd; pd = pd_tmp)
    {
        pd_tmp = pd->next;
        sdsfree(pd->target_task_id);
        sdsfree(pd->source_task_id);
        free(pd);
    }

    /* The dirty list holds session pointers only; sessions belong to session_manager, so only the array is freed here. */
    free(sched->dirty);
    sched->dirty = NULL;
    sched->dirty_count = 0;
    sched->dirty_cap = 0;

    /* The store is ours only when open() created it; a borrowed one (a test
     * fixture) belongs to its installer. Released last, after our own arrays are
     * gone, so no session pointer this object held outlives the sessions. */
    if (sched->sessions_owned && sched->sessions)
        edc_session_manager_free(sched->sessions);
    sched->sessions = NULL;
    sched->sessions_owned = false;

    if (sched->self)
        edc_actor_free(sched->self);
    free(sched);
}

/* ── Unresolved cross-session dependencies ─────────────────── */

void edc_scheduler_add_pending_dep(struct edc_scheduler *sched, const char *target_task_id,
                                   const char *source_task_id, edc_task_dep_type type, bool completed, bool cancelled,
                                   bool injected)
{
    if (!sched || !target_task_id || !source_task_id)
        return;
    struct edc_pending_dep *pd = g_new0(struct edc_pending_dep, 1);
    pd->target_task_id = sdsnew(target_task_id);
    pd->source_task_id = sdsnew(source_task_id);
    pd->type = type;
    pd->completed = completed;
    pd->cancelled = cancelled;
    pd->injected = injected;
    pd->next = NULL;

    /* Append at the tail: the reconcile cursor walks the list and may append while resolving. */
    if (!sched->pending_deps)
    {
        sched->pending_deps = pd;
        return;
    }
    struct edc_pending_dep *tail = sched->pending_deps;
    while (tail->next)
        tail = tail->next;
    tail->next = pd;
}

bool edc_scheduler_task_has_unresolved_deps(struct edc_scheduler *sched, struct edc_task *task)
{
    return edc_scheduler_first_unresolved_dep(sched, task) != NULL;
}

const char *edc_scheduler_first_unresolved_dep(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !task || !task->task_id)
        return NULL;
    for (struct edc_pending_dep *pd = sched->pending_deps; pd; pd = pd->next)
    {
        if (pd->target_task_id && strcmp(pd->target_task_id, task->task_id) == 0)
            return pd->source_task_id;
    }
    return NULL;
}

edc_actor_t *edc_scheduler_get_actor(struct edc_scheduler *sched)
{
    return sched ? sched->self : NULL;
}

/* ── Task/Group management ─────────────────────────────────── */

char *edc_scheduler_gen_task_id(struct edc_scheduler *sched)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)sched->next_task_id++);
    /* Persist on issue, so a reclaimed or killed task's id is never reused. */
    sched_persist_task_seq(sched);
    return strdup(buf);
}

struct edc_task *edc_scheduler_task_new(struct edc_scheduler *sched, struct edc_session *session, const char *desc,
                                           edc_channel_type channel_type)
{
    if (!sched || !session)
        return NULL;

    char *tid = edc_scheduler_gen_task_id(sched);
    /* Owned by the passed session; no session -> reject rather than guess. */
    const char *sid = session->session_id ? session->session_id : "sess-1";
    struct edc_task_group *fg = sched_foreground(session);
    const char *gid = fg ? fg->group_id : tid;

    struct edc_task *task = edc_task_new(tid, sid, gid, desc, channel_type);
    /* edc_task_new already leaves it READY; no forced WAITING_INPUT. */
    free(tid);
    if (!task)
        return NULL;

    /* L2: a new task's "seen" watermark = the current head (it gets the full index on round one). */
    edc_session_memory_adopt_task(session, task);

    /* Attach the raw input being handled (fork and system tasks do not). */
    if (sched->input_raw)
        task->raw_input = sdsnew(sched->input_raw);

    /* A new task changed the tree: mark dirty so it shows in the sidebar. */
    sched_mark_dirty(sched, session, "task");

    return task;
}

/* ── Task creation with placement ──────────────────────────── */

/* Choose the group a task lands in and join it. The single answer to "where does a
 * new task go" -- create / fork / system all settle through here instead of each
 * deciding for itself. */
/* The ONE place that decides "a maintenance round holds this task": attach the barrier edges
 * at the two doors that can make a task runnable (creation, resume). Everything else follows
 * from the dep graph -- a task with an unresolved edge is not runnable, and a BARRIER is
 * released by its source ending either way. There is deliberately no drive-time predicate any
 * more: two mechanisms deciding the same thing is how they drift apart. */
static int sched_attach_maintenance_barrier(struct edc_task *task)
{
    return edc_group_fold_hold_task(task) + edc_session_memory_hold_task(task);
}

static struct edc_task_group *sched_settle_task(struct edc_scheduler *sched, struct edc_session *session,
                                                struct edc_task *task, const struct edc_task_spec *spec,
                                                bool *chained_out)
{
    struct edc_task_group *group = NULL;

    if (chained_out)
        *chained_out = false;

    switch (spec->place)
    {
    case EDC_TASK_PLACE_SYSTEM:
        task->sink = spec->sink;
        group = edc_session_find_system_group(session);
        if (group)
        {
            edc_task_group_add_task(group, task);
        }
        else
        {
            group = edc_task_group_new(task);
            if (!group)
                return NULL;
            group->kind = EDC_GROUP_KIND_SYSTEM;
            edc_session_add_group(session, group);
        }
        break;

    case EDC_TASK_PLACE_GROUP:
        /* The caller names the group; an unknown name makes one led by this task. */
        group = spec->group_id ? edc_session_find_group(session, spec->group_id) : NULL;
        if (group)
            edc_task_group_add_task(group, task);
        else
            group = edc_scheduler_create_group(sched, session, task);
        break;

    case EDC_TASK_PLACE_NEW_GROUP:
        group = edc_scheduler_create_group(sched, session, task);
        break;

    case EDC_TASK_PLACE_FOREGROUND:
    default:
        group = sched_foreground(session);
        /* A finished, orphaned or system group is not a place to put new work. */
        if (!group || group->is_orphaned || group->member_count == 0 || edc_task_group_is_system(group))
            group = edc_scheduler_create_group(sched, session, task);
        else
            edc_task_group_add_task(group, task);
        break;
    }

    /* One in front at a time: a task joining the group that is currently in front waits
     * for whatever is still unfinished there. The edge is an order constraint (no output
     * is injected), but its failure semantics are a dependency's: a predecessor that dies
     * takes this task with it and keeps the edge, so resuming this one puts it back behind
     * the predecessor instead of jumping the queue. A predecessor that finished satisfies
     * it at once. A background task (`&` / `--bg`) is not queued -- it asked to run
     * alongside. */
    if (!spec->background)
    {
        bool chained = sched_chain_behind_front(task);
        if (chained_out)
            *chained_out = chained;
    }

    /* Door 1 of 2: a task that joins while a maintenance round is in flight waits for it too
     * (this is what the former drive-time predicate caught for newcomers). */
    sched_attach_maintenance_barrier(task);

    return group;
}

bool edc_scheduler_task_adopt(struct edc_scheduler *sched, struct edc_session *session, struct edc_task *task,
                              const struct edc_task_spec *spec)
{
    if (!sched || !session || !task || !spec)
        return false;

    /* A background task never takes the screen -- not at creation (take_foreground is
     * false for them) and not when it is driven (see sched_drive_task). */
    task->is_background = spec->background;

    /* Edges first: a dependent parks in WAITING_DEPS, and the group it joins does
     * not care which phase it was in. */
    for (size_t i = 0; i < spec->dep_count; i++)
        edc_task_add_dependency(task, spec->deps[i], TASK_DEP_JOIN);
    if (spec->prev)
        edc_task_add_dependency(task, spec->prev, spec->prev_type);

    bool chained = false;
    struct edc_task_group *group = sched_settle_task(sched, session, task, spec, &chained);
    if (!group)
    {
        log_error("task %s: nowhere to land (group not creatable)", task->task_id ? task->task_id : "?");
        return false;
    }

    if (spec->take_foreground)
    {
        /* The group is where the next task goes. The active pointer (what the frontend
         * shows live) moves onto the new task -- except when it was queued behind the one
         * in front: that one is running, and taking the pointer away would black out its
         * output; the drive rule hands the pointer over when the queued task runs. */
        edc_scheduler_set_foreground(session, group);
        if (!chained)
            edc_session_set_active_task(session, task);
    }

    /* Attaching the input is all that happens here; running it is the message-end
     * scan's job. (Parking is not a creation option: an interrupted task lands in
     * INTERRUPTED, and /task resume undoes it -- that is a phase change, not a flag.) */
    if (spec->input)
    {
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup(task->task_id);
        edc_turn_add_user_message(turn, spec->input);
        task->active_turn = turn;
    }

    /* A seeded starting tool call (`!cmd`): mark the task tool-only, put the
     * assistant(tool_calls) message into its turn, and register the call PENDING in the
     * ledger. Nothing is dispatched here -- the end-of-message scan drives the task and the
     * executor's tool-only branch sends the call, exactly as it would for a call the model
     * had asked for. From there the ordinary tool path runs it. */
    if (spec->initial_tool_name)
    {
        task->tool_only = true;

        sds call_id = sdscatprintf(sdsempty(), "call_local_%s", task->task_id ? task->task_id : "0");
        if (task->active_turn)
            edc_turn_add_assistant_tool_call(task->active_turn, call_id, spec->initial_tool_name,
                                             spec->initial_tool_args);
        edc_task_tool_call_add(task, call_id, spec->initial_tool_name, spec->initial_tool_args);
        sdsfree(call_id);
    }

    return true;
}

struct edc_task *edc_scheduler_task_create(struct edc_scheduler *sched, struct edc_session *session,
                                           const struct edc_task_spec *spec)
{
    if (!sched || !session || !spec)
        return NULL;

    struct edc_task *task =
        edc_scheduler_task_new(sched, session, (spec->desc && *spec->desc) ? spec->desc : "New task",
                               spec->channel_type);
    if (!task)
        return NULL;

    if (!edc_scheduler_task_adopt(sched, session, task, spec))
    {
        edc_task_free(task);
        return NULL;
    }

    return task;
}

struct edc_task_group *edc_scheduler_create_group(struct edc_scheduler *sched, struct edc_session *session,
                                                  struct edc_task *leader)
{
    if (!sched || !session || !leader)
        return NULL;
    struct edc_task_group *group = edc_task_group_new(leader);
    edc_scheduler_add_group(sched, session, group);
    /* L2: a leader arriving this way may have been built by create_task (adopting
     * twice is harmless) or directly by fork (edc_task_fork skips create_task); both
     * must be adopted. */
    edc_session_memory_adopt_task(session, leader);
    return group;
}

void edc_scheduler_add_group(struct edc_scheduler *sched, struct edc_session *session, struct edc_task_group *group)
{
    if (!sched || !group || !session)
        return;
    edc_session_add_group(session, group);
}

/* system task: real work (one fold LLM call), an ordinary persisted task, and
 * visible (rendered in its own sidebar block). The group is created with this task
 * as leader when none exists. */
struct edc_task *edc_scheduler_spawn_system_task(struct edc_scheduler *sched, struct edc_session *session,
                                                 const char *desc)
{
    /* Default = an L1 fold task (output goes to group memory); L2 curation uses the sink-taking version below. */
    return edc_scheduler_spawn_system_task_as(sched, session, desc, EDC_SINK_GROUP_MEMORY);
}

struct edc_task *edc_scheduler_spawn_system_task_as(struct edc_scheduler *sched, struct edc_session *session,
                                                    const char *desc, edc_sink sink)
{
    if (!sched || !session)
        return NULL;

    struct edc_task_group *group = edc_session_find_system_group(session);

    char *tid = edc_scheduler_gen_task_id(sched);
    const char *gid = group ? group->group_id : tid;
    struct edc_task *task = edc_task_new(tid, session->session_id, gid, desc, edc_channel_adapter);
    free(tid);
    if (!task)
        return NULL;
    edc_session_memory_adopt_task(session, task);

    /* Two attributes: the output goes to some memory (fold = group, curation =
     * session), and the container is the system group. lifetime defaults to PERSISTENT
     * -- these tasks must survive a restart so an unfinished round resumes. No turn:
     * the caller attaches one. */
    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.place = EDC_TASK_PLACE_SYSTEM;
    spec.sink = sink;
    if (!edc_scheduler_task_adopt(sched, session, task, &spec))
    {
        edc_task_free(task);
        return NULL;
    }

    /* Make it immediately visible in the sidebar (it may be the only thing running). */
    sched_mark_dirty(sched, session, "system-task");
    return task;
}

bool edc_scheduler_has_pending_system_task(struct edc_session *session)
{
    struct edc_task_group *group = edc_session_find_system_group(session);
    if (!group)
        return false;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (!edc_task_is_finished(t))
            return true;
    }
    return false;
}

bool edc_scheduler_attach_input(struct edc_scheduler *sched, struct edc_task *task, const char *input)
{
    (void)sched;
    if (!task || !input)
        return false;

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup(task->task_id);
    /* A bare user message (role/content only); decorations belong to the display path. */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", input);
    edc_queue_push(&turn->messages, msg);
    task->active_turn = turn;
    return true;
}

/* Drive = hand a runnable task to the executor's START and pass the result to the
 * policy point. Request ids and registration live here; the next I/O is the
 * executor's. */
static void *sched_gen_request_id(void)
{
    /* Start high so generated ids never collide with small literals (pointer-keyed hash). */
    static int64_t seq = (int64_t)1 << 42;
    return (void *)(intptr_t)(++seq);
}

/* Runnable = has work and nobody is driving it; deps are read off step, not the edges.
 * A fold in flight for the task's group is a barrier: the group's work is held
 * until the fold lands (see edc_group_fold_holds_group). The fold task itself is
 * in the session's system group, so it is not held by its own fold. */
static bool sched_task_runnable(struct edc_scheduler *sched, struct edc_task *task)
{
    /* "May this run" is decided by the task's OWN edges (plus the cross-session ledger). A task
     * parked at AWAIT_DEPS is already excluded by is_ready; this term keeps the invariant true
     * for a READY task that somehow still carries an unmet edge (the barrier is attached at
     * creation and at resume, and every consumer must agree the edge is what holds it). */
    return task && edc_task_is_ready(task) && task->active_turn &&
           !edc_task_has_unresolved_deps(task) && !edc_scheduler_task_has_unresolved_deps(sched, task);
}

static void sched_drive_task(struct edc_scheduler *sched, struct edc_task *task);
static void sched_save_session(struct edc_scheduler *sched, struct edc_session *s);

/* Fold trigger mount point (b): just before a task is ABOUT TO SEND a request, ask
 * once "is its group due to fold?", and if so start the fold -- edc_group_fold_start
 * hangs a SEQUENCE dep on every non-terminal member (this task included), so the
 * predicate below immediately parks it in AWAIT_DEPS. The barrier adds no mechanism
 * (B1): an in-flight member parks at the next request boundary via tool_done, and the
 * fold task is dispatched by the message-boundary scan as usual.
 * Mount point (a) is at the tail of edc_scheduler_apply_advance (checked after every
 * advance). */
static void sched_fold_before_request(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !sched->sessions || !task || !task->group || !task->active_turn)
        return;
    /* Only a task about to send a request triggers (a sink=GROUP_MEMORY fold task does not fold itself). */
    if (!edc_task_is_ready(task) || edc_task_is_memory_sink(task))
        return;
    /* A tool-only task (`!cmd`) sends no context at all -- its round is one tool call --
     * so folding for it would burn an LLM call on nothing. */
    if (task->tool_only)
        return;

    struct edc_session *session = task->group->session;
    /* `has_pending_system_task` is session-wide, so it already covers "a fold/curation for any
     * group is in flight" -- the former per-group term here could never decide anything. */
    if (!session || edc_scheduler_has_pending_system_task(session))
        return;
    if (!edc_group_fold_due(task->group, sched->config))
        return;
    if (edc_group_fold_start(sched, session, task->group, NULL))
        sched_save_session(sched, session); /* the fold task and its dep edges must be persistable at once */
}

struct edc_task *edc_scheduler_fold_group_now(struct edc_scheduler *sched, struct edc_session *session,
                                              struct edc_task_group *group, const char *focus)
{
    struct edc_task *fold = edc_group_fold_start(sched, session, group, focus);
    if (fold)
    {
        /* (c) Manual fold = the user asking for one more try: clear the consecutive
         * failure count (the only restart switch once the automatic path hits the cap). */
        edc_group_fold_note_progress(group);
        sched_save_session(sched, session); /* as on the automatic path: persistable at once */
    }
    return fold;
}

/* Scan callback: ask each task "can it run?" once; a successful drive = progress this round. */
struct sched_sweep_ctx
{
    struct edc_scheduler *sched;
    size_t driven;
};

static void sched_schedule_visit(void *ctx, struct edc_task *task)
{
    struct sched_sweep_ctx *sweep = ctx;
    /* Check folding before the request: starting a fold makes the predicate below false (a new dep), parking the task in AWAIT_DEPS. */
    sched_fold_before_request(sweep->sched, task);
    if (!sched_task_runnable(sweep->sched, task))
        return;
    sched_drive_task(sweep->sched, task);
    sweep->driven++;
}

/* Scheduling: a full-table scan at the end of every inbound message -- there is no
 * tick, so the message boundary is the tick. It must NOT run after a single
 * advance: one message may contain several (a group cancel, a failure cascade),
 * and driving then would start a downstream task halfway through a cancel.
 * The predicate is the state, not the cause. Scan to a fixed point: a failed drive
 * broadcasts and may release SEQUENCE dependents earlier in the scan order. */
void edc_scheduler_schedule(struct edc_scheduler *sched)
{
    if (!sched || !sched->sessions)
        return;
    size_t driven;
    do
    {
        struct sched_sweep_ctx sweep = {.sched = sched, .driven = 0};
        edc_session_manager_foreach_task(sched->sessions, sched_schedule_visit, &sweep);
        driven = sweep.driven;
    } while (driven > 0);
}

/* Hand START to the executor, then pass the facts to the policy point. If nothing
 * was started, take the just-issued request_id back. */
static void sched_drive_task(struct edc_scheduler *sched, struct edc_task *task)
{
    /* What the frontend shows live is whoever is running in the group that is in front.
     * A task never takes the front away from another one that is already running (that
     * is what `&` asked for), so a task that was queued behind the one in front picks
     * the active pointer up here -- when it finally runs and nothing is in front. */
    struct edc_session *own = edc_task_session(task);
    if (own && task->group && !task->is_background && own->foreground_group == task->group)
    {
        struct edc_task *cur = edc_session_get_active_task(own);
        if (cur != task && (!cur || !edc_task_is_running(cur)))
            edc_session_set_active_task(own, task);
    }

    void *req_id = sched_gen_request_id();
    edc_scheduler_register_task(sched, task, req_id);
    task->current_request_id = req_id;

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_START;
    ev.request_id = (const char *)req_id;

    struct edc_advance_result res = edc_executor_advance(&sched->executor, task, &ev);

    if (!res.settled && !edc_task_is_running(task))
    {
        edc_scheduler_unregister_task(sched, req_id);
        task->current_request_id = NULL;
    }
    edc_scheduler_apply_advance(sched, task, req_id, &res);
}

/* ── Lookup ────────────────────────────────────────────────── */

struct edc_task *edc_scheduler_find_task(struct edc_scheduler *sched, void *request_id)
{
    if (!sched || !request_id)
        return NULL;
    struct edc_sched_task_entry *entry;
    HASH_FIND_PTR(sched->task_hash, &request_id, entry);
    return entry ? entry->task : NULL;
}

struct edc_task *edc_scheduler_find_task_by_id(struct edc_scheduler *sched, struct edc_session *session,
                                               const char *task_id)
{
    if (!sched || !task_id || !session)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        struct edc_task *task = edc_task_group_find_task(group, task_id);
        if (task)
            return task;
    }
    return NULL;
}

/* ── Foreground/background ─────────────────────────────────── */

void edc_scheduler_set_foreground(struct edc_session *session, struct edc_task_group *group)
{
    /* The foreground group is a session property (terminal tab). */
    struct edc_session *owner = group ? group->session : session;
    if (owner)
        owner->foreground_group = group;
}

struct edc_task_group *edc_scheduler_get_foreground(struct edc_session *session)
{
    return sched_foreground(session);
}

/* The task an id-less command acts on: the foreground group's last unfinished member.
 * Deliberately not `active_task` -- that one is the screen focus and may sit on a task
 * that already finished (the final reply is delivered *after* the task reaches
 * COMPLETED, see edc_executor.c), which makes it a poor default for /task stop, /task
 * resume and `{{SELF}}`. */
struct edc_task *edc_scheduler_current_task(struct edc_session *session)
{
    struct edc_task_group *fg = sched_foreground(session);
    struct edc_task *last = NULL;
    struct edc_task *t;

    if (!fg)
        return NULL;

    LL_FOREACH2(fg->members, t, group_next)
    {
        if (!edc_task_is_finished(t))
            last = t;
    }
    return last;
}

struct edc_task *edc_scheduler_resumable_task(struct edc_session *session)
{
    struct edc_task_group *fg = sched_foreground(session);
    struct edc_task *last = NULL;
    struct edc_task *t;

    if (!fg)
        return NULL;

    LL_FOREACH2(fg->members, t, group_next)
    {
        if (!edc_task_is_completed(t))
            last = t;
    }
    return last;
}

const char *edc_scheduler_task_workdir(struct edc_task *task)
{
    /* Runs in the task's own bound directory; NULL when it has no owner. */
    struct edc_session *own = (task && task->group) ? task->group->session : NULL;
    return own ? edc_session_get_workdir(own) : NULL;
}

/* ── Register/unregister ───────────────────────────────────── */

void edc_scheduler_register_task(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    /* Registering a new request clears `cancelled_request`, or a resumed task drops
     * its own new response. Late responses of the old request are dropped by id. */
    if (task)
        task->cancelled_request = 0;

    struct edc_sched_task_entry *e = g_new0(struct edc_sched_task_entry, 1);
    e->request_id = request_id;
    e->task = task;
    HASH_ADD_PTR(sched->task_hash, request_id, e);
}

void edc_scheduler_unregister_task(struct edc_scheduler *sched, void *request_id)
{
    struct edc_sched_task_entry *e;
    HASH_FIND_PTR(sched->task_hash, &request_id, e);
    if (e)
    {
        HASH_DEL(sched->task_hash, e);
        free(e);
    }
}

/* ── Signals ───────────────────────────────────────────────── */

/* A task that just got cancelled takes its running tools with it: the child is a detached
 * process, and a user-typed command has NO timeout by design, so if the toolbox is not
 * told, a cancelled `!tail -f` would run until the app exits. The toolbox owns the
 * in-flight calls, so it is asked by request id (never by task pointer: the tool side must
 * not dereference a task that may already be reclaimed). */
static void sched_abort_cancelled_tools(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    if (sched->abort_tools && edc_task_is_cancelled(task))
        sched->abort_tools(sched, request_id);
}

void edc_scheduler_send_signal_to_task_ptr(struct edc_scheduler *sched, struct edc_task *task, edc_task_signal sig)
{
    if (!sched || !task)
        return;

    /* The executor applies the signal; here we only build the event. */
    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_SIGNAL;
    ev.as.signal.sig = sig;
    /* A signal carries no request_id: unregister the one the task remembers. */
    void *req_id = task->current_request_id;
    struct edc_advance_result res = edc_executor_advance(&sched->executor, task, &ev);
    sched_abort_cancelled_tools(sched, task, req_id);
    edc_scheduler_apply_advance(sched, task, req_id, &res);
}

void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, struct edc_session *session, const char *task_id,
                                       edc_task_signal sig)
{
    struct edc_task *task = edc_scheduler_find_task_by_id(sched, session, task_id);
    if (!task)
        return;
    edc_scheduler_send_signal_to_task_ptr(sched, task, sig);
}

/* Signal a task: the task layer moves the phase, then the host's unified entry does
 * the bookkeeping. Serves host-driven fan-out (group/session cancel). */
static void sched_signal_one(struct edc_scheduler *sched, struct edc_task *task, edc_task_signal sig)
{
    edc_task_step step_before = task->step;
    edc_task_life life_before = task->life;

    edc_task_send_signal(task, sig);

    /* Phase unchanged = a duplicate cancel: nothing settled. */
    if (task->step == step_before && task->life == life_before)
        return;

    /* Cancel = explicit termination: unregister, reap, dirty, settle, all in one entry. */
    void *req_id = task->current_request_id;
    sched_abort_cancelled_tools(sched, task, req_id);
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_DONE;
    res.changed = true;
    res.settled = true;
    edc_scheduler_apply_advance(sched, task, req_id, &res);
}

/* Signal a group: walk each member through the single-task path above. */
static void sched_signal_group_members(struct edc_scheduler *sched, struct edc_task_group *group, edc_task_signal sig,
                                       bool running_only)
{
    if (!sched || !group)
        return;
    struct edc_task *task, *next;
    LL_FOREACH_SAFE2(group->members, task, next, group_next)
    {
        if (running_only && !edc_task_is_running(task))
            continue;
        sched_signal_one(sched, task, sig);
    }
    /* The group's own wrap-up (leader cancelled -> mark orphaned) belongs to the group module. */
    edc_task_group_finish_signal(group);
}

void edc_scheduler_send_signal_to_group(struct edc_scheduler *sched, struct edc_session *session, const char *group_id,
                                        edc_task_signal sig)
{
    if (!sched || !session)
        return;
    struct edc_task_group *group = NULL;
    DL_FOREACH(session->groups, group)
    {
        if (strcmp(group->group_id, group_id) == 0)
            break;
    }
    sched_signal_group_members(sched, group, sig, false);
}

void edc_scheduler_send_signal_to_session(struct edc_scheduler *sched, struct edc_session *session, edc_task_signal sig,
                                          bool running_only)
{
    if (!sched || !session)
        return;
    struct edc_task_group *group, *tmp;
    DL_FOREACH_SAFE(session->groups, group, tmp)
    {
        sched_signal_group_members(sched, group, sig, running_only);
    }
}

void edc_scheduler_send_signal_to_all(struct edc_scheduler *sched, struct edc_session *session, edc_task_signal sig)
{
    edc_scheduler_send_signal_to_session(sched, session, sig, false);
}

/* ── resume ────────────────────────────────────────────────── */

void edc_scheduler_resume_task(struct edc_scheduler *sched, struct edc_task *task)
{
    if (!sched || !task)
        return;

    /* Door 2 of 2: a maintenance round in flight holds it. Park at READY/ALIVE -- the intent to
     * resume is NOT dropped: the message-end scan drives it as soon as the round ends and
     * releases the barrier (same effect as the fold's own member sweep, for the doors that sweep
     * cannot see). */
    if (sched_attach_maintenance_barrier(task) > 0)
    {
        /* AWAIT_DEPS is the ONE representation of "waiting for something": an edge added to a
         * READY task parks it there by itself, and the release path (the edge being completed or
         * voided) is written against that state. Parking at READY instead would leave a task that
         * is runnable-in-shape but held, which is exactly the drift this change removes. */
        edc_task_mark_waiting_deps(task);
        edc_scheduler_task_moved(sched, task);
        return;
    }

    /* Gate: only between-tasks checks stay here. An unresolved cross-session dep
     * holds the task back; unsatisfied deps send it back to waiting. */
    if (edc_scheduler_task_has_unresolved_deps(sched, task))
        return;

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
        if (completed < total)
        {
            /* Host-initiated move: same bookkeeping through the unified entry. */
            edc_task_mark_waiting_deps(task);
            edc_scheduler_task_moved(sched, task);
            return;
        }
    }

    /* Resume is the other door into "this task is about to run in the foreground group":
     * it must not jump the queue (creation goes through sched_settle_task for this). */
    if (sched_chain_behind_front(task))
    {
        edc_task_mark_waiting_deps(task);
        edc_scheduler_task_moved(sched, task);
        return;
    }

    /* Check folding before the request (mount point (b)): starting a fold hangs a dep
     * on this task, parking it in AWAIT_DEPS; after the fold lands the scan drives it
     * again, and that request then carries the compressed context. */
    sched_fold_before_request(sched, task);

    /* Drive: the within-task re-decision is the executor's START. */
    sched_drive_task(sched, task);
}


/* ── Foreground task state notification ────────────────────── */

/* A phase change only marks dirty; the push is merged into one per message by the
 * actor handler, since these transitions produce no channel message but the TUI is
 * snapshot-driven. Writes always state the target (step, life). */

/* Force a phase, skipping the transition table: a fallback exit, landing at READY. */
static void sched_force_move_task(struct edc_scheduler *sched, struct edc_task *task, edc_task_step step,
                                  edc_task_life life)
{
    edc_task_force_move(task, step, life);
    if (sched)
        sched_mark_dirty(sched, edc_task_session(task), "state");
}

/* ── Internals: message routing ────────────────────────────── */

static void sched_handle_input(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    const struct edc_sched_input *input = msg->data;
    if (!input)
        return;

    /* The session comes from the message itself; unresolvable -> reject. */
    struct edc_session *session = sched_resolve_session(sched, input->session_id);
    if (!session)
    {
        log_warn("input dropped: no session for tag '%s'", input->session_id ? input->session_id : "(none)");
        return;
    }

    /* Record the raw text being handled (cleared on every exit of this function). */
    sched->input_raw = input->content;

    /* Slash commands and the `+` new-group form go to the command handler; anything else
     * creates a task. (`@` is no longer a sigil -- it is the file-reference one, and the
     * handler answers it before this fallback could ever see it.) */
    const bool is_plain = input->content && input->content[0] != '/' && input->content[0] != '+';

    /* The command handler takes core's inbound input (origin for the raw message). */
    if (sched->handle_command && sched->handle_command(sched, msg->from, input))
    {
        sched->input_raw = NULL;
        return;
    }

    /* No command handler (or unhandled) -> create a task carrying this input.
     *
     * Production does not reach this: edc_commands_handler claims every non-`/` input,
     * so it is the seam the scheduler tests drive (they install a mock handler that
     * declines). Kept, and kept on the same placement as a real plain input. */
    sds effective_input = NULL;
    if (is_plain)
    {
        /* Plain input gets the same python-like quoting stripped as the shell path. */
        struct edc_cmd_task_opts popts;
        edc_cmd_parse_task_opts(input->content, &popts);
        if (popts.input && sdslen(popts.input) > 0)
            effective_input = sdsdup(popts.input);
        edc_cmd_task_opts_free(&popts);
    }

    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.desc = input->content;
    spec.channel_type = input->type;
    spec.input = effective_input ? effective_input : input->content;
    spec.place = EDC_TASK_PLACE_FOREGROUND;
    spec.take_foreground = true;
    edc_scheduler_task_create(sched, session, &spec);
    if (effective_input)
        sdsfree(effective_input);

    sched->input_raw = NULL; /* this input is done */
}

/* Output-redirect persistence lives in edc_task.c's edc_task_flush_redirects(). */

/* The executor's outbound ops. Each checks the callback is installed first. */

static bool ex_send_llm(void *ctx, struct edc_task *task, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    /* An uninstalled port is the fact "cannot make progress" -- never swallowed. */
    if (!sched->send_llm)
        return false;
    sched->send_llm(sched, task, request_id);
    return true;
}

static void ex_dispatch_tools(void *ctx, struct edc_task *task, void *llm_resp, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    if (sched->dispatch_tools)
        sched->dispatch_tools(sched, task, llm_resp, request_id);
}

static int ex_redispatch_tools(void *ctx, struct edc_task *task, void *request_id)
{
    struct edc_scheduler *sched = ctx;
    if (sched->redispatch_tools)
        return sched->redispatch_tools(sched, task, request_id);
    return 0;
}

static const struct edc_executor_ops sched_executor_ops = {
    .send_llm = ex_send_llm,
    .dispatch_tools = ex_dispatch_tools,
    .redispatch_tools = ex_redispatch_tools,
};

/* ── Edges between tasks (dependency DAG) ──────────────────── */

/* A queued dependent is started by the message-end scan, not from here. */

/* ── Cross-container wrap-up ───────────────────────────────── */

/* Intra-package call into the store: persistence is the store's job; this only hands over whom to save (NULL = active). */
static void sched_save_session(struct edc_scheduler *sched, struct edc_session *s)
{
    if (!sched || !sched->sessions)
        return;
    edc_session_store_save_session_state(sched->sessions, sched, s);
}

/* The "no API key" text points the user at /connect: presentation, not advance. */
static sds sched_error_copy(struct edc_scheduler *sched, const char *notice)
{
    const char *err = notice ? notice : "LLM request failed";
    const sds key = (sched && sched->config) ? sched->config->llm_api_conf.api_key : NULL;
    if (!key || sdslen(key) == 0)
    {
        /* Without an API key the request can only fail, so point at /connect. */
        return sdscatprintf(sdsempty(),
                            "%s\n\nNo API key configured — run /connect to set the API URL, model and key.", err);
    }
    return sdsnew(err);
}

/* Settle callback for side-effect-moved tasks: their facts come from the task's
 * current state. Recursion is intentional (settling can move more tasks). */
static void sched_settle_cascaded(void *ctx, struct edc_task *task)
{
    struct edc_scheduler *sched = ctx;
    bool finished = edc_task_is_finished(task);

    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = finished ? EDC_STEP_DONE : EDC_STEP_BLOCKED;
    res.changed = true;
    res.settled = finished;
    /* A cascaded task carries its own reason (do not fall back to a generic message). */
    res.notice = task->output;
    edc_scheduler_apply_advance(sched, task, task->current_request_id, &res);
}

/* Bookkeeping + settle for one task (steps 1-3). The public entry adds step 4. */
static void sched_apply_advance_one(struct edc_scheduler *sched, struct edc_task *task, void *req_id,
                                    const struct edc_advance_result *res)
{
    if (!sched || !task || !res)
        return;
    struct edc_session *own = edc_task_session(task);

    /* (1) Index: reaching a terminal state means no request is owed -- that task_hash slot can be reaped. */
    if (res->settled && req_id)
        edc_scheduler_unregister_task(sched, req_id);

    /* (2) Phase changed: mark the tree dirty. */
    if (res->changed)
    {
        sched_mark_dirty(sched, own, "state");
    }

    if (!res->settled)
        return;

    /* (3) Settle: reaching a terminal state -> six "tell the outside" actions, in the same order as before the move. */
    if (edc_task_is_completed(task))
    {
        /* Completion seq within the owning container; a GROUP_MEMORY task takes none. */
        if (task->sink == EDC_SINK_CHANNEL && own)
        {
            edc_task_set_completion_seq(task, own->next_completion_seq++);
            /* (c) New committed content in the group => clear the fold failure count:
             * this is a new situation worth retrying (distinct from the same content
             * failing repeatedly). Same for the extraction failure streak. */
            edc_group_fold_note_progress(task->group);
            edc_session_memory_extract_note_progress(task->group);
        }
        else
            edc_task_set_completion_seq(task, 0);

        /* The container grew: notify once, or a silent turn commit would stall the count. */
        sched_mark_dirty(sched, own, "complete");

        /* Notify dependents; starting the queued is the scan's job. */
        edc_task_notify_dependents(task);

        /* Delivery by sink: CHANNEL replies, GROUP_MEMORY writes the group's memory,
         * SESSION_MEMORY writes session entries (ops contract, fail-closed). */
        if (task->sink == EDC_SINK_GROUP_MEMORY)
        {
            bool landed = edc_group_fold_deliver_memory(task, res->notice);
            /* L2 is built AT FOLD START (with the fold; chain: members -> L2 ->(JOIN)
             * -> L1). One corner to note: the fold completed but wrote no memory =>
             * the memory is unchanged, yet L2 still runs (its JOIN edge is done) and
             * re-curates the old memory -- the curator merges the entries, so it is
             * not wrong, just a wasted call. */
            if (!landed)
                log_warn("L1 fold: group %s landed nothing — the chained curation will see the previous memory",
                         task->sink_group ? task->sink_group->group_id : "?");
        }
        else if (task->sink == EDC_SINK_SESSION_MEMORY)
        {
            /* Curation and compaction write through the memory tools DURING their run
             * so there is no post-hoc landing
             * step. Its final text is just a wrap-up and must not be sent to the user.
             * Extraction is the exception: its only post-hoc action is advancing the
             * memo watermark by the cut it carried. A clean STOP reaches here; a
             * truncated/failed one took the ERROR branch instead (the executor fails a
             * memory sink on any non-stop reason), and an iteration-cap stop comes in
             * COMPLETED but with a non-DONE outcome, so it is skipped here. */
            if (task->memo_kind == EDC_MEMO_KIND_EXTRACT)
            {
                if (res->outcome == EDC_STEP_DONE)
                    edc_session_memory_extract_land(task);
                else
                    edc_session_memory_extract_note_failure(task->sink_group);
            }
            else
                log_info("L2 memo done: task=%s (writes already landed via the memory tools)",
                         task->task_id ? task->task_id : "(null)");
        }
        else if (sched->send_reply)
            sched->send_reply(sched, task, res->notice, EDC_CHANNEL_MESSAGE_RESP_TYPE, req_id);

        /* A task can end well and still have something to say about what just
         * happened (see `warning` in edc_advance_result): that goes out as a system
         * message, after the reply, and never edits the reply itself. */
        if (res->warning[0] && sched->send_reply)
        {
            sds copy = sdsnew(res->warning);
            sched->send_reply(sched, task, copy, EDC_CHANNEL_MESSAGE_SYS_TYPE, req_id);
            sdsfree(copy);
        }

        /* The settlement subscribers: persistence. The fold check itself lives at the
         * tail of edc_scheduler_apply_advance, so every advance asks once (see (a)). */
        sched_save_session(sched, own);
    }
    else if (edc_task_is_cancelled(task))
    {
        /* An explicit cancel is persisted immediately, or a restart restores it as running. */
        sched_mark_dirty(sched, own, "foreground");
        /* An extraction round that was cancelled advanced nothing: count one failure so
         * a cancelling loop cannot become a retry loop. */
        if (task->sink == EDC_SINK_SESSION_MEMORY && task->memo_kind == EDC_MEMO_KIND_EXTRACT)
            edc_session_memory_extract_note_failure(task->sink_group);
        sched_save_session(sched, own);
    }
    else /* ERROR */
    {
        sched_mark_dirty(sched, own, "state");
        /* (c) A fold task reaching ERROR => count one failure; at the cap
         * edc_group_fold_due stops automatic folding (manual /group fold is exempt).
         * A cancel is not a failure -- a human stopped it. */
        if (task->sink == EDC_SINK_GROUP_MEMORY)
            edc_group_fold_note_failure(task->sink_group);
        if (task->sink == EDC_SINK_SESSION_MEMORY && task->memo_kind == EDC_MEMO_KIND_EXTRACT)
            edc_session_memory_extract_note_failure(task->sink_group);
        if (sched->send_reply)
        {
            sds copy = sched_error_copy(sched, res->notice);
            sched->send_reply(sched, task, copy, EDC_CHANNEL_MESSAGE_SYS_TYPE, req_id);
            sdsfree(copy);
        }
    }
}

/* edc_scheduler_apply_advance -- the one policy point after an advance.
 *
 * The executor reports facts; this turns "what happened" into "whom to tell":
 * index / reap / dirty / dependents / delivery / fold / persistence. Step 4 settles
 * the tasks an operation moved as a side effect. */
void edc_scheduler_apply_advance(struct edc_scheduler *sched, struct edc_task *task, void *req_id,
                                 const struct edc_advance_result *res)
{
    if (!sched || !task || !res)
        return;

    sched_apply_advance_one(sched, task, req_id, res);

    /* (4) Clear the accounts: recursive -- clearing itself moves other tasks. */
    edc_task_drain_moves(task, sched_settle_cascaded, sched);

    /* (5) Fold check (mount point (a)): asked after every advance, not only on the
     * "task completed" branch -- a tool-response advance can push a group's context
     * over the window while the task is still unfinished. Starting a fold hangs a
     * SEQUENCE dep on every non-terminal member; in-flight members park at the next
     * request boundary (B1, no new mechanism). The folding session may not be this
     * task's (background sessions fold too), so use the one try reports back. */
    if (sched->sessions)
    {
        struct edc_session *folded = NULL;
        if (edc_group_fold_try(sched, sched->sessions, sched->config, &folded) && folded)
            sched_save_session(sched, folded);
    }

    /* (5b) L2 extraction, asked at the same boundary. A fold has priority: it is
     * checked first above, and edc_session_memory_extract_due refuses while a fold is
     * due or in flight. Like the fold, the round it starts may belong to a background
     * session, so use the one try reports back. Its own completion is another message
     * boundary: apply_advance runs again, so a capped material continues until the
     * group has nothing left un-extracted. */
    {
        struct edc_session *extracted = NULL;
        if (edc_session_memory_extract_try(sched, sched->sessions, sched->config, &extracted) && extracted)
            sched_save_session(sched, extracted);
    }

    /* (6) L2 hygiene, asked at the same boundary: once a session's memory outgrows its entry
     * cap, start a compaction round. The decision is a fact about the data (entry_count vs
     * the cap, and whether it grew since the last round), so no write path had to set a flag
     * for it -- the same shape as the fold check above, and gated the same way (one system
     * task per session at a time). */
    {
        struct edc_session *own = edc_task_session(task);
        if (own && edc_session_memory_compact_due(own))
        {
            struct edc_task *compact = edc_session_memory_start_compact(sched, own);
            if (compact)
                sched_save_session(sched, own);
        }
    }
}

/* Host-initiated transitions go through the same bookkeeping. */
void edc_scheduler_task_moved(struct edc_scheduler *sched, struct edc_task *task)
{
    struct edc_advance_result res;
    memset(&res, 0, sizeof(res));
    res.outcome = EDC_STEP_BLOCKED;
    res.changed = true;
    edc_scheduler_apply_advance(sched, task, NULL, &res);
}

/* The scheduler only resolves the task by request_id and drops late cancelled events. */

static void sched_handle_llm_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;
    struct edc_advance_result res = edc_executor_advance_llm_resp(&sched->executor, task, msg->request_id, msg->data);
    edc_scheduler_apply_advance(sched, task, msg->request_id, &res);
}

static void sched_handle_tool_resp(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;
    struct edc_advance_result res = edc_executor_advance_tool_resp(&sched->executor, task, msg->request_id, msg->data);
    edc_scheduler_apply_advance(sched, task, msg->request_id, &res);

    /* A held send (a dep is outstanding) parked the task at AWAIT_DEPS without
     * sending: its request is over, so drop the mapping. It is re-driven with a
     * fresh id when the dep completes. */
    if (!res.settled && !edc_task_is_running(task) && task->current_request_id == msg->request_id)
    {
        edc_scheduler_unregister_task(sched, msg->request_id);
        task->current_request_id = NULL;
    }
}

static void sched_handle_tool_started(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;

    struct edc_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EDC_EVENT_TOOL_STARTED;
    ev.request_id = msg->request_id;
    ev.as.call_id = (const char *)msg->data;
    struct edc_advance_result res = edc_executor_advance(&sched->executor, task, &ev);
    edc_scheduler_apply_advance(sched, task, msg->request_id, &res);
}

/* Tool output echo (TOOL message): check the request is still alive via the task
 * hash (a late async callback's pointer is untrusted), require an owner session,
 * and refresh the todo dirty table. The display gate is in the frontend. */
static void sched_handle_tool_emit(struct edc_scheduler *sched, struct edc_actor_msg *msg)
{
    struct edc_tool_emit_view v;
    if (!edc_executor_view_tool_emit(msg->data, &v) || !v.content)
        return;

    struct edc_task *task = edc_scheduler_find_task(sched, msg->request_id);
    if (!task || task->cancelled_request)
        return;

    /* No owner means nowhere to deliver (no fallback to "current binding"). */
    struct edc_session *own = edc_task_session(task);
    if (!own)
        return;
    /* The display gate is in the frontend; core only states the fact. */

    if (sched->send_tool_message)
        sched->send_tool_message(sched, task, v.header, v.content, v.render_pref, msg->request_id);

    /* todowrite changes no task state, so push a snapshot to refresh the TodoWidget. */
    if (v.header && strcmp(v.header, "todowrite") == 0)
        sched_mark_dirty(sched, own, "todo");
}

/* ── Actor entry ───────────────────────────────────────────── */

/* Serialize the current session's task tree as JSON for the TUI sidebar (on the
 * scheduler thread, so race-free). EDGECLAW_SNAPSHOT_TRACE=<path> logs each push. */
static void sched_trace_snapshot(const char *reason, sds json)
{
    const char *path = getenv("EDGECLAW_SNAPSHOT_TRACE");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "pull reason=%s %s\n", reason ? reason : "?", json ? json : "");
    fclose(f);
}

/* The "event" half of the same trace: who changed, and why. */
static void sched_trace_notify(const char *reason, const char *session_id)
{
    const char *path = getenv("EDGECLAW_SNAPSHOT_TRACE");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "notify reason=%s session=%s\n", reason ? reason : "?", session_id ? session_id : "?");
    fclose(f);
}


/* The frontend named a tree to pull. An unresolvable name yields nothing (the frontend gets "no reply", not somebody else's tree). */
static void sched_handle_pull_snapshot(struct edc_scheduler *sched, const char *session_id)
{
    struct edc_session *session = sched_resolve_session(sched, session_id);
    if (!session)
    {
        log_warn("snapshot request dropped: no session for tag '%s'", session_id ? session_id : "(none)");
        return;
    }
    sched_handle_task_snapshot(sched, session, "pull");
}

static void sched_handle_task_snapshot(struct edc_scheduler *sched, struct edc_session *session, const char *reason)
{
    if (!sched || !session)
        return;

    cJSON *root = cJSON_CreateObject();
    struct edc_session *sess = session;
    cJSON_AddStringToObject(root, "session_id", sess->session_id ? sess->session_id : "");
    cJSON_AddStringToObject(root, "session_name", edc_session_get_name(sess));
    /* session messages + every task's in-flight turn messages. */
    size_t msg_count = edc_session_get_message_count(sess);
    {
        struct edc_task_group *g;
        DL_FOREACH(sess->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                if (t->active_turn)
                    msg_count += edc_queue_length(&t->active_turn->messages);
            }
        }
    }
    cJSON_AddNumberToObject(root, "message_count", (double)msg_count);
    cJSON_AddStringToObject(root, "created_at", sess->created_at ? sess->created_at : "");
    /* The session's bound working directory ("" when unbound). The frontend shows
     * it in the footer and must not reach into the store for it. */
    const char *sess_wd = edc_session_get_workdir(sess);
    cJSON_AddStringToObject(root, "workdir", sess_wd ? sess_wd : "");

    cJSON *groups = cJSON_AddArrayToObject(root, "groups");
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        cJSON *gj = cJSON_CreateObject();
        cJSON_AddStringToObject(gj, "id", g->group_id ? g->group_id : "");
        /* A pure projection: the scheduler neither reads nor filters on it. */
        cJSON_AddStringToObject(gj, "kind", edc_task_group_kind_name(g->kind));
        cJSON_AddNumberToObject(gj, "member_count", g->member_count);
        cJSON_AddBoolToObject(gj, "orphaned", g->is_orphaned);
        cJSON_AddBoolToObject(gj, "fg", sched_foreground(session) == g);
        /* Committed history (minus history[0]) + its tasks' in-flight turns. */
        size_t g_msg_count = g->history ? (size_t)cJSON_GetArraySize(g->history) : 0;
        if (g_msg_count > 0)
            g_msg_count -= 1;
        long long g_tokens_total = 0;
        long long g_tokens_prompt = 0;
        long long g_tokens_completion = 0;
        long long g_tokens_cached = 0;
        {
            struct edc_task *mt;
            LL_FOREACH2(g->members, mt, group_next)
            {
                if (mt->active_turn)
                    g_msg_count += edc_queue_length(&mt->active_turn->messages);
                g_tokens_total += mt->tokens_total;
                g_tokens_prompt += mt->tokens_prompt;
                g_tokens_completion += mt->tokens_completion;
                g_tokens_cached += mt->tokens_cached;
            }
        }
        /* The group's *current* context: the widest member conversation, as the
         * last reply's real usage plus an estimate of what was appended after it
         * (or a from-scratch estimate while no reply is trustworthy). Estimated
         * travels with the number -- the sidebar marks it, it never passes for a
         * provider measurement. */
        bool g_ctx_estimated = false;
        long long g_ctx_tokens = edc_context_group_ctx_tokens(g, &g_ctx_estimated);
        cJSON_AddNumberToObject(gj, "msg_count", (double)g_msg_count);
        cJSON_AddNumberToObject(gj, "tokens_total", (double)g_tokens_total);
        cJSON_AddNumberToObject(gj, "tokens_prompt", (double)g_tokens_prompt);
        cJSON_AddNumberToObject(gj, "tokens_completion", (double)g_tokens_completion);
        cJSON_AddNumberToObject(gj, "tokens_cached", (double)g_tokens_cached);
        cJSON_AddNumberToObject(gj, "ctx_tokens", (double)g_ctx_tokens);
        cJSON_AddBoolToObject(gj, "ctx_estimated", g_ctx_estimated);

        cJSON *tasks = cJSON_AddArrayToObject(gj, "tasks");
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            cJSON *tj = cJSON_CreateObject();
            cJSON_AddStringToObject(tj, "id", t->task_id ? t->task_id : "");
            cJSON_AddStringToObject(tj, "state", edc_task_state_str(t));
            cJSON_AddBoolToObject(tj, "leader", t->is_group_leader);
            cJSON_AddBoolToObject(tj, "active", sess->active_task == t);
            cJSON_AddStringToObject(tj, "desc", t->description ? t->description : "");
            /* The raw input that created the task ("" when none). */
            cJSON_AddStringToObject(tj, "input", t->raw_input ? t->raw_input : "");
            /* Token usage for the sidebar (double, exact below 2^53). */
            cJSON_AddNumberToObject(tj, "tokens_prompt", (double)t->tokens_prompt);
            cJSON_AddNumberToObject(tj, "tokens_completion", (double)t->tokens_completion);
            cJSON_AddNumberToObject(tj, "tokens_total", (double)t->tokens_total);
            cJSON_AddNumberToObject(tj, "tokens_cached", (double)t->tokens_cached);
            /* Todos travel with the snapshot; the sidebar reads them from it alone. */
            if (t->todos && cJSON_GetArraySize(t->todos) > 0)
                cJSON_AddItemToObject(tj, "todos", cJSON_Duplicate(t->todos, 1));
            cJSON_AddItemToArray(tasks, tj);
        }

        cJSON_AddItemToArray(groups, gj);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return;

    sds out = sdsnew(json);
    cJSON_free(json);

    sched_trace_snapshot(reason, out);

    /* A reply goes only to the frontend that NAMED it (the pull path); there is no push. */
    if (sched->snapshot_reply)
        sched->snapshot_reply(sched, out);
    else
        sdsfree(out);
}

void edc_scheduler_actor_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    struct edc_scheduler *sched = actor->state;
    if (!sched || !msg)
        return;

    switch (msg->type)
    {
    case BC_ACTOR_MSG_CHANNEL_RECV:
        sched_handle_input(sched, msg);
        break;
    case BC_ACTOR_MSG_LLM_RESPONSE:
        sched_handle_llm_resp(sched, msg);
        break;
    case BC_ACTOR_MSG_TOOL_RESPONSE:
        sched_handle_tool_resp(sched, msg);
        break;
    case BC_ACTOR_MSG_TOOL_STARTED:
        sched_handle_tool_started(sched, msg);
        break;
    case BC_ACTOR_MSG_TOOL_EMIT:
        sched_handle_tool_emit(sched, msg);
        break;
    case BC_ACTOR_MSG_SERVICE_PUMP:
        /* A service timer fired: run whichever services are due. This is the only place
         * a service body runs, so it is still "at a message boundary". */
        edc_services_pump(sched, sched->self && sched->self->loop ? uv_now(sched->self->loop) : 0);
        break;
    case BC_ACTOR_MSG_TASK_SNAPSHOT:
        /* A pull names which tree it wants; nothing pushes a tree unasked. */
        sched_handle_pull_snapshot(sched, (const char *)msg->data);
        break;
    default:
        break;
    }

    /* One push per message at most, before msg is freed. */
    /* Every effect has landed; drive by state (a pure pull reaches here too). */
    edc_scheduler_schedule(sched);

    sched_drain_dirty(sched);

    edc_actor_msg_free(msg);
}

/* Message history of the selected task/group (source task->turns), or NULL. */

static void history_append_messages(const struct edc_task *task, cJSON *messages)
{
    for (size_t i = task->turns.head; i < task->turns.tail; i++)
    {
        const struct edc_turn *turn = (const struct edc_turn *)task->turns.data[i];
        for (size_t j = turn->messages.head; j < turn->messages.tail; j++)
            cJSON_AddItemToArray(messages, cJSON_Duplicate((cJSON *)turn->messages.data[j], 1));
    }
}

/* One member block in a group: {"id", "input", "messages"}. */
static cJSON *history_task_block(const struct edc_task *task)
{
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "id", task->task_id ? task->task_id : "");
    /* The raw input that created this task: what the user actually typed. */
    cJSON_AddStringToObject(block, "input", task->raw_input ? task->raw_input : "");
    history_append_messages(task, cJSON_AddArrayToObject(block, "messages"));
    return block;
}

char *edc_scheduler_task_history_json(struct edc_scheduler *sched, struct edc_session *session,
                                      const char *group_id, const char *task_id)
{
    if (!sched || !session || (!group_id && !task_id))
        return NULL;

    struct edc_task_group *g = NULL;
    if (group_id && *group_id)
    {
        DL_FOREACH(session->groups, g)
        {
            if (!g->group_id || strcmp(g->group_id, group_id) != 0)
                continue;
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "kind", "group");
            cJSON_AddStringToObject(root, "id", group_id);
            cJSON *blocks = cJSON_AddArrayToObject(root, "tasks");
            struct edc_task *t = NULL;
            LL_FOREACH2(g->members, t, group_next)
                cJSON_AddItemToArray(blocks, history_task_block(t));
            char *out = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            return out;
        }
        return NULL;
    }

    /* A single task: use the existing authoritative lookup (it knows group members and cross-session lazy loading). */
    struct edc_task *t = edc_scheduler_find_task_by_id(sched, session, task_id);
    if (!t)
        return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "kind", "task");
    cJSON_AddStringToObject(root, "id", t->task_id ? t->task_id : "");
    cJSON_AddStringToObject(root, "input", t->raw_input ? t->raw_input : "");
    history_append_messages(t, cJSON_AddArrayToObject(root, "messages"));
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}
