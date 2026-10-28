#ifndef __BC_SESSION_MANAGER_H__
#define __BC_SESSION_MANAGER_H__

#include "sds.h"
#include <stdint.h>
#include <cJSON.h>
#include <stdbool.h>

struct edc_session;
struct edc_config;
struct edc_task;
struct edc_scheduler;
struct edc_task_index_entry; /* uthash entry: task_id -> session_id */

struct edc_session_manager
{
    struct edc_session **sessions; /* dynamic array of session pointers */
    int session_count;
    int active_index; /* index into sessions[] */

    /* Session ids come from the database now (sessions.session_id is an INTEGER PRIMARY KEY
     * AUTOINCREMENT, see edc_state_store_insert_session_new): there is no counter here and
     * no <workspace>/.session_seq. AUTOINCREMENT is what guarantees an id is never reused --
     * reuse would make a new session inherit a removed one's directory and its chat.jsonl. */
    sds sessions_dir; /* workspace/ -- session dirs live here */
    struct edc_config *config;

    /* No task_id -> session_id index: it was a uthash filled by parsing every unloaded
     * session's state file, and the database answers the same question with one primary-key
     * lookup (edc_state_store_session_for_task). Nothing here to keep in sync after a save. */
};

extern struct edc_session_manager *edc_session_manager_new(struct edc_config *config);
extern void edc_session_manager_free(struct edc_session_manager *sm);

extern struct edc_session *edc_session_manager_get_active(struct edc_session_manager *sm);
extern struct edc_session *edc_session_manager_find(struct edc_session_manager *sm, const char *name_or_id);
extern int edc_session_manager_index_of(struct edc_session_manager *sm, const char *name_or_id);

/** Find a task by id across all loaded sessions. Task ids come from a process-wide
 *  monotonic counter persisted in <workspace>/.task_seq, so a hit is unambiguous.
 *  When |owner_out| is non-NULL it receives the owning session on success, or NULL
 *  on failure. */
extern struct edc_task *edc_session_manager_find_task_any(struct edc_session_manager *sm, int64_t task_id,
                                                          struct edc_session **owner_out);

/** Hand every task in memory to |cb| (session list x group list x member list).
 *  The only enumeration entry for full-table scans; the order is fixed. Do not add
 *  or remove tasks/groups from the callback, which is read-only. */
typedef void (*edc_session_task_visit_fn)(void *ctx, struct edc_task *task);
extern void edc_session_manager_foreach_task(struct edc_session_manager *sm, edc_session_task_visit_fn cb, void *ctx);

/** Owning session of |task_id| when it is not loaded yet, via the lazily-built
 *  index. Loaded sessions are not indexed here; callers search memory first. */
extern struct edc_session *edc_session_manager_session_for_task(struct edc_session_manager *sm, int64_t task_id);

/** Drop the cached task-id index (rebuilt on next lookup). Only needed when
 *  persisted state changed behind the manager's back (tests / external tooling). */
extern void edc_session_manager_reset_task_index(struct edc_session_manager *sm);

/** Create a session (auto-named when name is NULL/empty) and make it active. */
extern struct edc_session *edc_session_manager_create(struct edc_session_manager *sm, const char *name);

/** Like edc_session_manager_create, but binds |workdir| (an existing directory).
 *  A given-but-unusable directory fails creation. */
extern struct edc_session *edc_session_manager_create_ex(struct edc_session_manager *sm, const char *name,
                                                         const char *workdir);

/** Switch to a session by name or id, saving the outgoing session's history. Its
 *  state must be persisted by the caller first (the foreground group needs the
 *  scheduler). */
extern bool edc_session_manager_switch(struct edc_session_manager *sm, const char *name_or_id);

/** Close a session: save history, remove from the active list. Refuses to close
 *  the last remaining session. */
extern bool edc_session_manager_stop(struct edc_session_manager *sm, const char *name_or_id);

extern bool edc_session_manager_rename(struct edc_session_manager *sm, const char *name_or_id, const char *new_name);

extern sds edc_session_manager_list(struct edc_session_manager *sm);

/** Save the index (sessions.json). Session state is NOT part of this: it needs a
 *  scheduler, so it is written by edc_session_store_save_all_sessions_state() /
 *  edc_session_store_save_session_state_tagged() (src/kernel/session/edc_state_store.h). */
extern void edc_session_manager_save_all(struct edc_session_manager *sm);

/** Persist the state of every session LOADED in this run (exit path). Sessions
 *  taken from the index but never activated are skipped: their in-memory object is
 *  an id/name-only shell, so writing it would clobber newer on-disk state.
 *  |sched| may be NULL. */
extern void edc_session_manager_save_loaded_states(struct edc_session_manager *sm, struct edc_scheduler *sched);

/** Load sessions from sessions.json; creates a `default` session on first run. */
extern bool edc_session_manager_load(struct edc_session_manager *sm);

#endif
