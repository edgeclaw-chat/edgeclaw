#ifndef __BC_SCHEDULER_H__
#define __BC_SCHEDULER_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_executor.h"
#include "edc_msg_part.h" /* struct edc_msg_part：pass 追加的消息 */
#include "edc_task.h"
#include "edc_task_group.h"
#include "sds.h"
#include <stdbool.h>

struct edc_session;
struct edc_session_manager;
struct edc_config;
struct edc_service;

/* Core's own inbound vocabulary: the channel boundary translates a channel
 * message into this, so the scheduler never sees `struct edc_channel_message`. */
struct edc_sched_input
{
    const char *session_id; /* owning session label; NULL/empty = active session */
    const char *content;
    edc_channel_type type; /* channel kind; today only adapter (TUI) */
    void *origin;          /* original channel message, passed through to the command handler */
};

struct edc_sched_task_entry; /* uthash entry (request_id -> task); head pointer type only */

struct edc_scheduler;

typedef void (*edc_scheduler_send_llm_fn)(struct edc_scheduler *sched, struct edc_task *task, void *request_id);
typedef void (*edc_scheduler_send_reply_fn)(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                                            edc_channel_message_type message_type, void *request_id);
typedef void (*edc_scheduler_send_tool_fn)(struct edc_scheduler *sched, struct edc_task *task, const char *header,
                                           const char *content, edc_render_pref_t render_pref, void *request_id);
typedef void (*edc_scheduler_dispatch_tools_fn)(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                                void *request_id);
/** Replays the task's PENDING tool ledger; returns how many requests were posted. */
typedef int (*edc_scheduler_redispatch_tools_fn)(struct edc_scheduler *sched, struct edc_task *task, void *request_id);
/** Abort whatever tools are still running for |request_id| (the task was cancelled).
 *  Provided by the toolbox, which owns the in-flight calls: a cancelled task's child
 *  process must die with it, or a command with no timeout (`!cmd`) would run forever. */
typedef void (*edc_scheduler_abort_tools_fn)(struct edc_scheduler *sched, void *request_id);
/** Returns true when the command was handled. */
typedef bool (*edc_scheduler_command_fn)(struct edc_scheduler *sched, struct edc_actor *actor,
                                         const struct edc_sched_input *input);
/** Answer to a pull: the serialized tree the frontend asked for. */
typedef void (*edc_scheduler_snapshot_fn)(struct edc_scheduler *sched, sds serialized);
/** "The tree of session X changed" -- an event carrying no tree; the frontend
 *  pulls what it displays. `reason` is trace-only. */
typedef void (*edc_scheduler_snapshot_changed_fn)(struct edc_scheduler *sched, const char *session_id,
                                                  const char *reason);

/** One "tree changed" entry; `reason` is a static literal, not owned. */
struct edc_sched_dirty
{
    struct edc_session *session;
    const char *reason;
};

/** A cross-session dependency edge read from disk whose source task was missing
 *  from the loaded session's id_map. Kept by id so it cannot dangle. */
struct edc_pending_dep
{
    sds target_task_id;
    sds source_task_id;
    edc_task_dep_type type;
    bool completed;
    bool cancelled;
    bool injected;
    struct edc_pending_dep *next;
};

struct edc_scheduler
{
    struct edc_config *config;

    /* Steps a task from within: the stateless advance function plus the
     * outbound ops (see edc_executor.h). */
    struct edc_executor executor;

    struct edc_sched_task_entry *task_hash;

    /* Unresolved cross-session dependency edges, drained on load; an entry that
     * cannot be resolved holds its target (resume gate). `reconciling` keeps the
     * drain from re-entering through a lazy session load. */
    struct edc_pending_dep *pending_deps;
    bool reconciling;

    edc_actor_t *self;

    /* Session store. Owned by this object only when edc_scheduler_open() created
     * it (`sessions_owned`); otherwise it is borrowed -- tests hang their own
     * fixture manager here and free it themselves, so free must not touch it.
     * Either way the scheduler holds no session copy. */
    struct edc_session_manager *sessions;
    bool sessions_owned;
    bool sessions_closed; /* edc_scheduler_close() already ran (idempotence) */

    /* Ports, installed by their providers. */
    edc_scheduler_send_llm_fn send_llm;
    edc_scheduler_send_reply_fn send_reply;
    edc_scheduler_send_tool_fn send_tool_message;
    edc_scheduler_dispatch_tools_fn dispatch_tools;
    edc_scheduler_redispatch_tools_fn redispatch_tools;
    edc_scheduler_abort_tools_fn abort_tools;
    edc_scheduler_command_fn handle_command;
    edc_scheduler_snapshot_fn snapshot_reply;
    edc_scheduler_snapshot_changed_fn snapshot_changed;

    /* Raw text of the input being handled (not owned, valid during the message);
     * edc_scheduler_task_new attaches it to the new task. */
    const char *input_raw;

    /* Sessions whose tree changed, deduped by pointer, flushed at message end. */
    struct edc_sched_dirty *dirty;
    size_t dirty_count;
    size_t dirty_cap;

    int64_t next_task_id;

    /* Background services (see edc_service.h): a list of named "silent checks" and
     * ONE uv_timer shared by all of them, re-armed to the earliest next_due. The timer
     * is closed by the actor runner's shutdown walk, so nothing here is heap-owned. */
    struct edc_service *services;
    uv_timer_t service_timer;
    bool service_timer_inited;

    /* Opaque handle so tests can hang a fixture session on this scheduler. */
    void *ctx;
};

/* Record an unresolved cross-session edge (source absent from the loaded
 * session's id_map); copied by id and drained by the reconcile pass. */
extern void edc_scheduler_add_pending_dep(struct edc_scheduler *sched, const char *target_task_id,
                                          const char *source_task_id, edc_task_dep_type type, bool completed,
                                          bool cancelled, bool injected);

/** True while |task| has an unresolved cross-session dep, i.e. it must not be
 *  resumed yet (the resume gate). */
extern bool edc_scheduler_task_has_unresolved_deps(struct edc_scheduler *sched, struct edc_task *task);

/** Source id of |task|'s first unresolved cross-session dep, or NULL. */
extern const char *edc_scheduler_first_unresolved_dep(struct edc_scheduler *sched, struct edc_task *task);

extern struct edc_scheduler *edc_scheduler_new(struct edc_config *config);
extern void edc_scheduler_free(struct edc_scheduler *sched);

/** Create the session store and load the active session. Called once, at assembly
 *  time: the config is ready and the runner has not started yet. Returns false
 *  when the store cannot be created or its index cannot be read (the caller
 *  aborts startup), or when this scheduler already holds a store.
 *
 *  This is the only entry that may create a session manager; `edc_scheduler_new`
 *  stays infallible and in-memory on purpose, because the store touch is disk I/O
 *  that can fail. Tests that want a store keep installing their own fixture on
 *  `sched->sessions` -- `sessions_owned` stays false and free leaves it alone. */
extern bool edc_scheduler_open(struct edc_scheduler *sched);

/** Register the kernel's built-in background services (today: the idle-group
 *  extraction scan) and arm the shared timer. Called by the assembly root once the
 *  scheduler's actor has a loop (i.e. after edc_actor_runner_add). No-op when the
 *  config disables services. Tests without a loop may call it and then drive
 *  `edc_services_pump` with a fake clock. */
extern void edc_scheduler_services_start(struct edc_scheduler *sched);

/** Shut the process down: persist every session as-is, then drop the in-flight
 *  turns. Does NOT free anything -- that is edc_scheduler_free's job.
 *
 *  Ordering contract: the caller MUST have stopped its writers first (for the app,
 *  that means after edc_actor_runner_free) or the save races the actor thread.
 *  Idempotent; false when there is no store open.
 *
 *  The app remains the one who decides *when* this happens: "the runner stopped"
 *  is process-lifecycle knowledge the scheduler does not have. */
extern bool edc_scheduler_close(struct edc_scheduler *sched);

extern edc_actor_t *edc_scheduler_get_actor(struct edc_scheduler *sched);

extern char *edc_scheduler_gen_task_id(struct edc_scheduler *sched);

/** Message history of a task or group as JSON. Has no callers yet. `task_id`
 *  selects one task, else `group_id` selects every member. Returns a malloc'd
 *  string (caller frees) or NULL when not found. */
extern char *edc_scheduler_task_history_json(struct edc_scheduler *sched, struct edc_session *session,
                                             const char *group_id, const char *task_id);
/** Fold an existing decimal task id into the allocator's high-water mark and
 *  persist it, so ids issued after a restart stay above the saved maximum. */
extern void edc_scheduler_observe_task_id(struct edc_scheduler *sched, const char *task_id);

/** The bare task object: id, owner session, L2 watermark, raw input. Makes NO group
 *  membership and attaches NO turn -- placement is edc_scheduler_task_create's job
 *  below. Kept separate because the three constructors (new / fork / system) differ
 *  here and only here. */
extern struct edc_task *edc_scheduler_task_new(struct edc_scheduler *sched, struct edc_session *session,
                                               const char *desc, edc_channel_type channel_type);

/* ── Task creation with placement ───────────────────────────
 *
 * The one entry a front-end uses: object -> edges -> group -> turn. Splitting
 * "where does a new task land" out of each caller is what keeps it from being
 * answered five different ways (an earlier round had five copies: /task new, the
 * shell spawn, fork, the system task, and the plain-input fallback). */

/** Where a new task lands. */
typedef enum
{
    /** The session's foreground group; one is created (this task leading it) when
     *  the session has none, or the current one is finished / orphaned. */
    EDC_TASK_PLACE_FOREGROUND,
    /** The group named by spec->group_id, created (this task leading it) when the
     *  session has no such group. */
    EDC_TASK_PLACE_GROUP,
    /** A fresh group led by this task. */
    EDC_TASK_PLACE_NEW_GROUP,
    /** The session's system group (fold / curation), created when it has none. */
    EDC_TASK_PLACE_SYSTEM,
} edc_task_place;

struct edc_task_spec
{
    const char *desc;
    edc_channel_type channel_type;

    /* The turn this task runs on. NULL attaches none (system tasks, and any caller
     * that builds its own turn); an empty string still attaches an empty turn. */
    const char *input;
    /* Extra messages the model sees next to |input|, in order (an @file's contents, and
     * whatever the next pass adds). Borrowed, not owned: the caller keeps the AST alive
     * for the duration of creation, and the turn copies each content.
     *
     * A LIST, not one field per kind: a new pass appends to it, and creation just loops.
     * Kept apart from |input| on purpose -- the request phase expands {{…}} in USER
     * messages, so an expansion's bytes must never share a message with them. */
    const struct edc_msg_part *parts;
    int nparts;

    edc_task_place place;
    const char *group_id; /* EDC_TASK_PLACE_GROUP */
    /* Move the session's foreground group and active pointer onto this task.
     * Background and system tasks do not take it. */
    bool take_foreground;
    /* The caller said "run this alongside" (`&` / `--bg`). Such a task is not queued
     * behind the group's current holder; everything else that lands in the group in
     * front waits its turn. Distinct from !take_foreground: `--group <the group in
     * front>` also leaves the front alone, but it IS part of that conversation and must
     * queue. */
    bool background;
    edc_sink sink; /* EDC_TASK_PLACE_SYSTEM: where this task's output goes */

    /* TASK_DEP_JOIN sources, already resolved: turning an id into a task is the
     * front-end's job (the id may name a task of another session). */
    struct edc_task **deps;
    size_t dep_count;
    /* The ';' / '|' edge source -- the previous command of the same command line.
     * That pointer exists only inside the shell's AST walk, so only the shell can
     * pass it (NULL for a lone command). */
    struct edc_task *prev;
    edc_task_dep_type prev_type;

    /* A seeded starting tool call: when set, the task is created `tool_only` (it never
     * talks to the LLM) with one PENDING call already in its ledger and the matching
     * `assistant(tool_calls)` message already in its turn. Driving it then walks the
     * ordinary tool path -- dispatch, result into the turn, settle -- with no LLM round
     * at all. `args` is the JSON argument object (copied; caller keeps ownership).
     * First user: `!cmd` (a user-typed shell command as a task). */
    const char *initial_tool_name;
    cJSON *initial_tool_args;
};

/** Create a task and place it: object -> edges -> group -> foreground -> turn.
 *  NULL when there is no session or the group cannot be made. */
extern struct edc_task *edc_scheduler_task_create(struct edc_scheduler *sched, struct edc_session *session,
                                                  const struct edc_task_spec *spec);

/** The half of the above that runs once the object exists: edges -> group ->
 *  foreground -> turn. `spec->desc` is not read here (it only names the object the
 *  caller built), so a creator with a different object -- `edc_task_fork`, the
 *  system task -- can still settle through this one and share the placement.
 *  Returns false when the task could not be placed (nothing else is undone). */
extern bool edc_scheduler_task_adopt(struct edc_scheduler *sched, struct edc_session *session,
                                     struct edc_task *task, const struct edc_task_spec *spec);

/** Create a system task in the session's system group (creating it if needed):
 *  sink GROUP_MEMORY, group kind SYSTEM. Returns at
 *  (READY, ALIVE) with no input; the caller attaches a turn and the scheduler
 *  decides when it runs. NULL when there is no session. */
/* Spawn a system task by sink (L1 fold = GROUP_MEMORY, L2 curation =
 * SESSION_MEMORY); edc_scheduler_spawn_system_task is the sink = GROUP_MEMORY
 * legacy entry. */
extern struct edc_task *edc_scheduler_spawn_system_task_as(struct edc_scheduler *sched, struct edc_session *session,
                                                           const char *desc, edc_sink sink);
extern struct edc_task *edc_scheduler_spawn_system_task(struct edc_scheduler *sched, struct edc_session *session,
                                                        const char *desc);

/** True while the session's system group still holds a system task, i.e. a fold
 *  is already in flight. */
extern bool edc_scheduler_has_pending_system_task(struct edc_session *session);

/** Manual fold: `/group fold [id] [focus]` goes through here. It skips the "due"
 *  predicate (a manual fold must happen now) but keeps every barrier (never
 *  interrupt an in-flight fold, one per session). Persists immediately after
 *  starting, like the automatic path. Returns the started fold task, or NULL when
 *  none started (nothing to fold / a fold already in flight). */
extern struct edc_task *edc_scheduler_fold_group_now(struct edc_scheduler *sched, struct edc_session *session,
                                                     struct edc_task_group *group, const char *focus);

/** Attach input (a turn) to a task. Does not dispatch -- whether it runs is the
 *  scheduler's decision (see edc_scheduler_schedule). */
extern bool edc_scheduler_attach_input(struct edc_scheduler *sched, struct edc_task *task, const char *input);

extern struct edc_task_group *edc_scheduler_create_group(struct edc_scheduler *sched, struct edc_session *session,
                                                         struct edc_task *leader);
/** Attach `group` to `session`. Both are needed because a group is owned by its
 *  session; it does not remember the owner itself. */
extern void edc_scheduler_add_group(struct edc_scheduler *sched, struct edc_session *session,
                                    struct edc_task_group *group);

extern struct edc_task *edc_scheduler_find_task(struct edc_scheduler *sched, void *request_id);
/** Find a task by id within |session|. Cross-session lookups go through the
 *  store's task index. */
extern struct edc_task *edc_scheduler_find_task_by_id(struct edc_scheduler *sched, struct edc_session *session,
                                                      const char *task_id);

/** The task a command with no id acts on: the foreground group's last unfinished
 *  member, or NULL when there is no foreground group or all of its members have
 *  finished. Deliberately NOT `session->active_task` -- that one is the screen focus
 *  and may sit on a task that already finished, which makes it a poor default for
 *  "/task stop", "/task status" and `{{SELF}}`. */
extern struct edc_task *edc_scheduler_current_task(struct edc_session *session);

/** The foreground group is a property of the session, not the scheduler. */
extern void edc_scheduler_set_foreground(struct edc_session *session, struct edc_task_group *group);
extern struct edc_task_group *edc_scheduler_get_foreground(struct edc_session *session);

/** Workdir for a tool call: that of the calling task's owning session, so a
 *  background task runs in its own directory. NULL when the task has no owner. */
extern const char *edc_scheduler_task_workdir(struct edc_task *task);

/** True when |session| holds a task that has not finished. */

extern void edc_scheduler_register_task(struct edc_scheduler *sched, struct edc_task *task, void *request_id);
extern void edc_scheduler_unregister_task(struct edc_scheduler *sched, void *request_id);

/** Bookkeeping after the host changed a task's state itself (index + dirty
 *  table), for paths that bypass the executor's advance. */
extern void edc_scheduler_task_moved(struct edc_scheduler *sched, struct edc_task *task);

/** The one policy point after an advance: turns the facts in
 *  `struct edc_advance_result` into indexes / dirty / dependents /
 *  delivery / fold / persistence. Every executor drive must call it with the
 *  result, or that bookkeeping is lost. */
extern void edc_scheduler_apply_advance(struct edc_scheduler *sched, struct edc_task *task, void *req_id,
                                       const struct edc_advance_result *res);

extern void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, struct edc_session *session,
                                              const char *task_id, edc_task_signal sig);
/** Same as the id-based variant, on an already-resolved task pointer. */
extern void edc_scheduler_send_signal_to_task_ptr(struct edc_scheduler *sched, struct edc_task *task,
                                                  edc_task_signal sig);
extern void edc_scheduler_send_signal_to_group(struct edc_scheduler *sched, struct edc_session *session,
                                               const char *group_id, edc_task_signal sig);
/** Signal every group under |session|: true hits only edc_task_is_running()
 *  tasks (process exit), false clears every non-terminal task (/session remove). */
extern void edc_scheduler_send_signal_to_session(struct edc_scheduler *sched, struct edc_session *session,
                                                 edc_task_signal sig, bool running_only);
extern void edc_scheduler_send_signal_to_all(struct edc_scheduler *sched, struct edc_session *session,
                                             edc_task_signal sig);

/** Resume a stopped task (interrupted / suspended / failed / cancelled): dependency gate,
 *  then drive it
 *  through the executor's START (the same path a READY task takes). */
extern void edc_scheduler_resume_task(struct edc_scheduler *sched, struct edc_task *task);

/** Drive every runnable task (READY/ALIVE + turn + deps satisfied). Called at the
 *  end of every message -- there is no tick, the message boundary is the tick.
 *  Scans to a fixed point, since a failed drive may release new runnable tasks
 *  through its broadcast. */
extern void edc_scheduler_schedule(struct edc_scheduler *sched);

#endif
