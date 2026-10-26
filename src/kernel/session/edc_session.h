#ifndef __BC_SESSION_H__
#define __BC_SESSION_H__

#include "edc_error.h"
#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_config;
struct edc_session_memory;
struct edc_turn;

struct edc_timestamp_node
{
    sds ts;
    struct edc_timestamp_node *prev, *next;
};

/* ── Forward declarations for task/group ───────────────────── */
struct edc_task;
struct edc_task_group;

/* ── Session ───────────────────────────────────────────────── */

struct edc_session
{
    /* The sessions table key, assigned by the database (INTEGER PRIMARY KEY
     * AUTOINCREMENT) -- like task_id. 0 = not assigned yet. It used to be the TEXT "s<N>"
     * handed out by a counter whose high-water mark lived in <ws>/.session_seq. */
    int64_t session_id;
    char *name; /* user-friendly display name */
    /* Unix seconds, the one representation used everywhere (DB columns, sessions.json and
     * in memory). 0 = unknown. These used to be ISO strings, which meant save/load
     * converted back and forth and the same instant existed in two formats. */
    uint64_t created_at;
    uint64_t last_active;

    /* The single representation of task state (v3 on): groups/tasks. The old
     * session->messages track and v2 read compatibility are gone. */
    struct edc_task_group *groups; /* doubly-linked list: all process groups */
    struct edc_task *active_task;  /* the active task within the foreground group */

    /* This session's own foreground group (owner info, like group->session). It
     * travels with the session across switches, and a background session's persist
     * brings back the right foreground_group_id. */
    struct edc_task_group *foreground_group;

    /* Whether the in-memory groups/tasks are ready. First activation (startup, or
     * switching to a never-loaded session) reads the session's rows once; after that this
     * tree is the working copy and a switch back must never load again (that would rebuild
     * live objects from the database and discard unsaved changes). */
    bool state_loaded;

    /* Quarantine flag: set when the session HAS rows but they could not be read (a version
     * mismatch, a corrupt page, a SQL error). While set, this process REFUSES to persist --
     * otherwise the half-built shell would be flushed over the real rows, turning a
     * recoverable database into a plausibly-shaped empty one. See
     * edc_session_load_state() for where it is set. */
    bool state_quarantined;

    /* The legacy session.json warning was already emitted for this session. Nothing reads
     * that file any more; a leftover one is reported ONCE per session per process rather
     * than at every tool boundary. */
    bool legacy_state_warned;

    struct edc_config *config;

    /* L2: session-level memory (event table + entry table + digest watermark).
     * Never NULL (built at construction); persisted in the memo_events / memo_entries
     * tables plus a few counters on the session row. */
    struct edc_session_memory *memo;

    sds history_dir; /* workspace/<session_id> -- per-session history */

    /* The last `!cmd` typed in this session (`!!` runs it again). In memory only: it is
     * a convenience at the prompt, not state the session owns across a restart. */
    sds last_local_cmd;

    /* Bound working directory (absolute realpath). NULL = unset -> fall back to the
     * launch directory (config->sandbox_root_real). Tool execution (exec_shell's
     * default cwd, fs tools' relative paths) and the system prompt's {{cwd}} both
     * use it. */
    sds workdir;

    uint32_t next_completion_seq; /* completion-order counter (starts at 1) */
};

/* ── Lifetime ──────────────────────────────────────────────── */

/** Create a session with a specific id and display name. */
extern struct edc_session *edc_session_new(struct edc_config *config, int64_t session_id, const char *name);

/** Create a session bound to a working directory. |workdir| is resolved with
 *  realpath() and must name an existing directory; NULL/empty keeps the
 *  default (the launch directory). If |workdir| is given but unusable, creation
 *  FAILS (returns NULL) rather than silently binding the default. */
extern struct edc_session *edc_session_new_with_workdir(struct edc_config *config, int64_t session_id,
                                                        const char *name, const char *workdir);

/* The session's directory name, WITHOUT the workspace prefix: the id's text form, exactly
 * as edc_id_str() renders it ("15"), but as a string the caller owns.
 *
 * There is deliberately ONE text form for an id. An earlier version prefixed session ids
 * with an "s" (a leftover from the days when they were "s<N>" strings) while task and group
 * ids were bare numbers -- so "which of the two renderers is right here?" became a question
 * a developer had to remember, and getting it wrong compiled cleanly and silently wrote a
 * session's transcript into a directory that did not exist. Now the database's own type
 * decides: an id is an integer, so its text form is that integer.
 *
 * Caller frees with sdsfree. */
extern sds edc_session_dir_name(int64_t session_id);


/** The session's bound working directory as an absolute realpath, or NULL when
 *  the session is unbound (callers then fall back to the launch directory,
 *  config->sandbox_root_real). */
extern const char *edc_session_get_workdir(struct edc_session *session);

/** Resolve |workdir| to the absolute realpath of an existing directory.
 *  An absolute path is used as-is; a leading "~"/"~/" expands to the home
 *  directory; any other relative path resolves against |base| (NULL/empty → the
 *  process cwd), so a caller can anchor it at a session's current working
 *  directory. Returns a new sds (caller frees), or NULL with *error (when
 *  non-NULL) set to the reason: empty path, nonexistent path, or not a
 *  directory. This is the single place the "relative → absolute + must exist"
 *  rule lives. */
extern sds edc_session_resolve_workdir(const char *base, const char *workdir, EdcError **error);

/** Bind the session to |workdir| (absolute or relative to the process cwd).
 *  Resolves it with realpath(), requires an existing directory, and grants it
 *  read+write in the sandbox (edc_config_add_sandbox_rw). Pass NULL/empty to
 *  unbind (revert to the launch directory). Returns true on success; on failure
 *  *error (when non-NULL) carries the reason and the binding is unchanged. */
extern bool edc_session_set_workdir(struct edc_session *session, const char *workdir, EdcError **error);
extern void edc_session_free(struct edc_session *session);

/** Build the "/session status" report for |session|: group/task counts, the
 *  per-state task breakdown, and — when |is_current| — the foreground group
 *  (|foreground_group_id|, may be NULL) and the session's active task. Returns
 *  a new sds (caller frees). */
extern sds edc_session_status_text(struct edc_session *session, bool is_current, const char *foreground_group_id);

/** Reset session to empty state (clear messages, tasks, groups),
 *  keeping the same id/name. Does NOT save or load. */
extern void edc_session_reset(struct edc_session *session);

/** Get the display name (falls back to session_id). */
extern const char *edc_session_get_name(struct edc_session *session);

/* ── Messages / history ────────────────────────────────────── */

extern size_t edc_session_get_message_count(struct edc_session *session);

/* ── Task/Group management ─────────────────────────────────── */

extern void edc_session_add_group(struct edc_session *session, struct edc_task_group *group);
extern void edc_session_remove_group(struct edc_session *session, struct edc_task_group *group);
extern struct edc_task_group *edc_session_find_group(struct edc_session *session, int64_t group_id);

/** The session's SYSTEM group (kind == SYSTEM), or NULL when there is none.
 *
 *  The container of system tasks (folds etc.): created on demand by
 *  edc_scheduler_spawn_system_task and removed by the reclaim path when the last
 *  system task is reclaimed, so it may be absent at any
 *  moment and callers must handle NULL.
 *
 *  This is the only core-side read of `kind`, and it is structural ("which
 *  container these tasks belong to"): nothing is hidden, skipped or changed. The
 *  other reader is the display layer. */
extern struct edc_task_group *edc_session_find_system_group(struct edc_session *session);

extern struct edc_task *edc_session_find_task(struct edc_session *session, int64_t task_id);

extern void edc_session_set_active_task(struct edc_session *session, struct edc_task *task);
extern struct edc_task *edc_session_get_active_task(struct edc_session *session);

/* ── Full state persistence ────────────────────────────────── */

struct edc_scheduler;

/** Save the session's state into <workspace>/edgeclaw.db (see edc_state_store.h).
 *
 *  Not a file write any more: the flush upserts the rows that changed, in one transaction.
 *  (This used to say "writes to workspace/<session_id>/session.json, atomic via .tmp then
 *  rename()" -- that file is not written or read by anything now, only warned about when a
 *  leftover one is found. <session_id>/ holds the chat transcript and tool output.)
 *
 *  The caller must only save a session whose state was loaded in this run
 *  (session->state_loaded): an unloaded session is just an id/name shell built from the
 *  sessions table, so writing it would clobber the newer rows (groups, tasks, and the bound
 *  working directory). See edc_session_manager_save_loaded_states() for the exit path.
 *  Returns true on success. */
extern bool edc_session_save_state(struct edc_session *session, struct edc_scheduler *sched);

/** Same as edc_session_save_state(), but records |tag| (the call site) in the
 *  stage-0 baseline probe (EDGECLAW_STATE_PROBE, see edc_state_probe.h). Exists
 *  only so the baseline can tell the flush sites apart; behaves identically. */
extern bool edc_session_save_state_tagged(struct edc_session *session, struct edc_scheduler *sched,
                                          const char *tag);

/** Load session state from <workspace>/edgeclaw.db (see edc_state_store.h).
 *  Rebuilds all groups, tasks, turns, and dependencies.
 *  Returns true on success, false when the store holds nothing for this session
 *  (a brand-new session) or when reading failed. */
extern bool edc_session_load_state(struct edc_session *session, struct edc_scheduler *sched);

#endif
