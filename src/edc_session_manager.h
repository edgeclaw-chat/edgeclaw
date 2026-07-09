#ifndef __BC_SESSION_MANAGER_H__
#define __BC_SESSION_MANAGER_H__

#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_session;
struct edc_config;

/* ── Session Manager ───────────────────────────────────────── */

struct edc_session_manager {
    struct edc_session **sessions;  /* dynamic array of session pointers */
    int session_count;              /* number of active sessions */
    int active_index;               /* index into sessions[] */
    sds sessions_dir;               /* workspace/ — session dirs live here */
    sds index_path;                 /* workspace/sessions.json */
    struct edc_config *config;
};

/* ── Lifecycle ─────────────────────────────────────────────── */

extern struct edc_session_manager *edc_session_manager_new(struct edc_config *config);
extern void edc_session_manager_free(struct edc_session_manager *sm);

/* ── Access ────────────────────────────────────────────────── */

extern struct edc_session *edc_session_manager_get_active(struct edc_session_manager *sm);
extern struct edc_session *edc_session_manager_find(struct edc_session_manager *sm,
                                                     const char *name_or_id);
extern int edc_session_manager_index_of(struct edc_session_manager *sm,
                                         const char *name_or_id);

/* ── Operations ────────────────────────────────────────────── */

/** Create a new session (auto-named if name is NULL/empty).
 *  The new session becomes active. Returns the created session. */
extern struct edc_session *edc_session_manager_create(struct edc_session_manager *sm,
                                                       const char *name);

/** Switch to a session by name or id. Saves current session first. */
extern bool edc_session_manager_switch(struct edc_session_manager *sm,
                                        const char *name_or_id);

/** Stop (close) a session: save history, remove from active list.
 *  Refuses to stop the last remaining session. */
extern bool edc_session_manager_stop(struct edc_session_manager *sm,
                                      const char *name_or_id);

/** Rename a session. */
extern bool edc_session_manager_rename(struct edc_session_manager *sm,
                                        const char *name_or_id,
                                        const char *new_name);

/** List all sessions as a formatted string. */
extern sds edc_session_manager_list(struct edc_session_manager *sm);

/* ── Persistence ───────────────────────────────────────────── */

/** Save the index (sessions.json) and call save on active session. */
extern void edc_session_manager_save_all(struct edc_session_manager *sm);

/** Load sessions from sessions.json. On first run, migrates legacy
 *  workspace/sess-1/ into the multi-session index. */
extern bool edc_session_manager_load(struct edc_session_manager *sm);

#endif
