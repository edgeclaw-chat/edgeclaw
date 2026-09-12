#ifndef __BC_SESSION_MANAGER_H__
#define __BC_SESSION_MANAGER_H__

#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_session;
struct edc_config;
struct edc_task;

/* ── Session Manager ───────────────────────────────────────── */

struct edc_session_manager
{
    struct edc_session **sessions; /* dynamic array of session pointers */
    int session_count;             /* number of active sessions */
    int active_index;              /* index into sessions[] */
    sds sessions_dir;              /* workspace/ — session dirs live here */
    sds index_path;                /* workspace/sessions.json */
    struct edc_config *config;
};

/* ── Lifecycle ─────────────────────────────────────────────── */

extern struct edc_session_manager *edc_session_manager_new(struct edc_config *config);
extern void edc_session_manager_free(struct edc_session_manager *sm);

/* ── Access ────────────────────────────────────────────────── */

extern struct edc_session *edc_session_manager_get_active(struct edc_session_manager *sm);
extern struct edc_session *edc_session_manager_find(struct edc_session_manager *sm, const char *name_or_id);
extern int edc_session_manager_index_of(struct edc_session_manager *sm, const char *name_or_id);

/** Find a task by id across **all** sessions.
 *
 *  Task ids are issued by a process-wide monotonic counter persisted in
 *  <workspace>/.task_seq (see edc_scheduler_gen_task_id), so a task id is
 *  globally unique — across sessions and across restarts. A hit is therefore
 *  unambiguous. Only sessions already loaded into memory are searched
 *  (session->groups is populated lazily on first activation).
 *
 *  On success, when |owner_out| is non-NULL it receives the owning session
 *  (so callers can tell whether the hit is in the active session, route a
 *  signal/save to the right session, etc.). On failure |owner_out| is set to
 *  NULL and NULL is returned. */
extern struct edc_task *edc_session_manager_find_task_any(struct edc_session_manager *sm, const char *task_id,
                                                          struct edc_session **owner_out);

/* ── Operations ────────────────────────────────────────────── */

/** Create a new session (auto-named if name is NULL/empty).
 *  The new session becomes active. Returns the created session. */
extern struct edc_session *edc_session_manager_create(struct edc_session_manager *sm, const char *name);

/** Like edc_session_manager_create, but binds the session to |workdir| (an
 *  existing directory; NULL/empty keeps the default launch directory). A
 *  given-but-unusable directory makes creation fail (returns NULL). */
extern struct edc_session *edc_session_manager_create_ex(struct edc_session_manager *sm, const char *name,
                                                         const char *workdir);

/** Switch to a session by name or id. Saves the outgoing session's history;
 *  its state (which needs the scheduler for the foreground group) must be
 *  persisted by the caller before switching. */
extern bool edc_session_manager_switch(struct edc_session_manager *sm, const char *name_or_id);

/** Stop (close) a session: save history, remove from active list.
 *  Refuses to stop the last remaining session. */
extern bool edc_session_manager_stop(struct edc_session_manager *sm, const char *name_or_id);

/** Rename a session. */
extern bool edc_session_manager_rename(struct edc_session_manager *sm, const char *name_or_id, const char *new_name);

/** List all sessions as a formatted string. */
extern sds edc_session_manager_list(struct edc_session_manager *sm);

/* ── Persistence ───────────────────────────────────────────── */

/** Save the index (sessions.json) and call save on active session. */
extern void edc_session_manager_save_all(struct edc_session_manager *sm);

/** Load sessions from sessions.json. On first run, migrates legacy
 *  workspace/sess-1/ into the multi-session index. */
extern bool edc_session_manager_load(struct edc_session_manager *sm);

#endif
