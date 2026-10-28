#include "edc_session_manager.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_datetime_utils.h"
#include "edc_file_utils.h"
#include "edc_session.h"
#include "edc_state_store.h"
/* Both were pulled in transitively through edc_state_serializer.h until that file went
 * away; include what this file actually uses rather than relying on the next header to
 * do it. */
#include "edc_task.h"
#include "edc_task_group.h"
#include "log.h"
#include "sds.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <dirent.h>
#include <inttypes.h> /* PRId64 */
#include <errno.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────── */

/* ── Session number persistence ──────────────────────────────
 * A session id must NEVER be reused. If /session new after /session remove handed
 * back the same id, the new session would land on the deleted session's directory --
 * session.json / chat.jsonl / old sidecars left over from the previous lifecycle:
 * the TUI replays the old chat, and the cross-session task index attributes old
 * tasks to the wrong owner.
 *
 * Numbering at "max live session number + 1" (the old implementation) does not
 * prevent reuse: once a deleted session's number is the max, the next create takes
 * it back, and across a restart deleting the max number makes it regress too. So
 * the issuing high-water mark is persisted separately in workspace/.session_seq,
 * extended on every allocation; on load it takes the max of "file value / ids in the
 * index / existing session dirs under workspace", the last two covering pre-upgrade
 * archives and orphaned directories.
 * Pure stdio, no uv file loop (session manager unit tests do not init file utils). */

/* The session id is allocated by the database now (sessions.session_id is an INTEGER
 * PRIMARY KEY AUTOINCREMENT), so there is no counter, no <ws>/.session_seq, and no
 * "observe the directories to recover the high-water mark" pass. The invariant that file
 * existed for -- an id is NEVER reused, because a new session would otherwise land on a
 * removed session's directory and replay its chat.jsonl -- is what AUTOINCREMENT provides. */

static sds edc_sm_gen_session_name(struct edc_session_manager *sm)
{
    /* Generate a default name like "Session 2" */
    int max_n = 0;
    for (int i = 0; i < sm->session_count; i++)
    {
        const char *name = edc_session_get_name(sm->sessions[i]);
        if (name && strncmp(name, "Session ", 8) == 0)
        {
            int n = atoi(name + 8);
            if (n > max_n)
                max_n = n;
        }
    }
    return sdscatprintf(sdsempty(), "Session %d", max_n + 1);
}

/* ── Index persistence ─────────────────────────────────────── */

/* The session list is a QUERY now, not a file. sessions.json restated the `sessions` table
 * and the two could disagree -- an imported session sat invisible in the database until the
 * file was rewritten by hand. `is_active` was the one fact the table lacked; it is a column
 * now (with a partial unique index), so "where am I" survives a kill like everything else.
 *
 * What replaced edc_sm_save_index(): edc_session_manager_create / _switch / _stop each
 * write the one row they changed. Nothing rewrites the whole list. */

/* Persist which session is active. Called at every point that moves it. */
static void edc_sm_persist_active(struct edc_session_manager *sm)
{
    if (!sm || sm->active_index < 0 || sm->active_index >= sm->session_count)
        return;
    struct edc_session *s = sm->sessions[sm->active_index];
    if (!s || s->session_id <= 0)
        return;
    if (!edc_state_store_open(sm->config))
        return; /* no store (a hand-built test manager): memory only, as before */
    edc_state_store_set_active_session(s->session_id);
}

/* ── Load ──────────────────────────────────────────────────── */

bool edc_session_manager_load(struct edc_session_manager *sm)
{
    if (!sm)
        return false;

    /* The session list is the `sessions` table. sessions.json used to hold it too, and the
     * two copies could disagree -- an imported session sat in the database invisible until
     * the file was rewritten by hand. Opening the store here (idempotent) is what makes this
     * possible: the manager is built before anything else, so before this change there was
     * no database to ask. */
    if (!edc_state_store_open(sm->config))
    {
        log_error("session manager: cannot open the state store at %s",
                  sm->config && sm->config->workspace ? sm->config->workspace : "?");
        return false;
    }

    struct edc_session_row *rows = NULL;
    int n = 0;
    if (!edc_state_store_list_sessions(&rows, &n))
    {
        log_error("session manager: cannot read the session list");
        return false;
    }

    if (n == 0)
    {
        edc_session_rows_free(rows, 0);
        /* A brand-new workspace: one session, and it is the active one. */
        struct edc_session *def = edc_session_manager_create(sm, "default");
        if (!def)
            return false;
        sm->active_index = 0;
        edc_sm_persist_active(sm);
        return true;
    }

    sm->sessions = g_new0(struct edc_session *, n);
    sm->session_count = n;
    int active_idx = -1;
    for (int i = 0; i < n; i++)
    {
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%" PRId64, rows[i].session_id);
        struct edc_session *s = edc_session_new(sm->config, rows[i].session_id,
                                                rows[i].name && *rows[i].name ? rows[i].name : idbuf);
        if (!s)
        {
            edc_session_rows_free(rows, n);
            return false;
        }
        s->created_at = rows[i].created_at;
        s->last_active = rows[i].last_active;
        sm->sessions[i] = s;
        if (rows[i].is_active)
            active_idx = i;
    }
    edc_session_rows_free(rows, n);

    /* No recorded active session (a database written before is_active existed): pick the
     * newest and record the choice, so the next start is deterministic. */
    if (active_idx < 0)
        active_idx = n - 1;
    sm->active_index = active_idx;
    edc_sm_persist_active(sm);

    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────── */

struct edc_session_manager *edc_session_manager_new(struct edc_config *config)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->config = config;
    sm->active_index = -1;
    sm->sessions_dir = sdsnew(config->workspace);

    /* No high-water mark to restore and no directory scan to recover it: the database
     * allocates session ids and never reuses one. */
    if (!edc_session_manager_load(sm))
    {
        edc_session_manager_free(sm);
        return NULL;
    }

    /* Update last_active for the active session */
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (active)
    {
        active->last_active = (uint64_t)edc_datetime_now();
    }

    return sm;
}

void edc_session_manager_free(struct edc_session_manager *sm)
{
    if (!sm)
        return;

    /* Save before freeing */
    edc_session_manager_save_all(sm);

    for (int i = 0; i < sm->session_count; i++)
        edc_session_free(sm->sessions[i]);

    free(sm->sessions);
    sdsfree(sm->sessions_dir);
    free(sm);
}

/* ── Access ────────────────────────────────────────────────── */

struct edc_session *edc_session_manager_get_active(struct edc_session_manager *sm)
{
    if (!sm || sm->active_index < 0 || sm->active_index >= sm->session_count)
        return NULL;
    return sm->sessions[sm->active_index];
}

/* Does the TEXT the user typed spell the session with this id?
 *
 * An id's text form is its integer, and that is the only form recognised here. Anything else
 * -- including the old "s13" spelling -- falls through to the name comparison in the caller,
 * which is the correct answer: "s13" is not this session's id, and after the directories were
 * renamed it is not a name either.
 *
 * Keeping a prefix-stripping branch here bought nothing once the output side settled on one
 * form, and it was actively misleading: it implied the "s<id>" spelling still existed
 * somewhere. Tolerating an input format is only free while that format is still produced by
 * something. */
static bool edc_sm_id_matches(int64_t session_id, const char *typed)
{
    if (!typed || !*typed || session_id <= 0)
        return false;
    char *end = NULL;
    const long long v = strtoll(typed, &end, 10);
    return end != typed && end && *end == '\0' && v == session_id;
}

struct edc_session *edc_session_manager_find(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return NULL;
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (edc_sm_id_matches(s->session_id, name_or_id))
            return s;
        if (s->name && strcmp(s->name, name_or_id) == 0)
            return s;
    }
    return NULL;
}

int edc_session_manager_index_of(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return -1;
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (edc_sm_id_matches(s->session_id, name_or_id))
            return i;
        if (s->name && strcmp(s->name, name_or_id) == 0)
            return i;
    }
    return -1;
}

struct edc_task *edc_session_manager_find_task_any(struct edc_session_manager *sm, int64_t task_id,
                                                   struct edc_session **owner_out)
{
    if (owner_out)
        *owner_out = NULL;
    if (!sm || !task_id)
        return NULL;

    /* Task ids are globally unique, so the first hit is the only hit. Search
     * every in-memory session; a session that has not been activated yet has no
     * groups loaded and therefore nothing to match. */
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (!s)
            continue;
        struct edc_task *t = edc_session_find_task(s, task_id);
        if (t)
        {
            if (owner_out)
                *owner_out = s;
            return t;
        }
    }
    return NULL;
}

void edc_session_manager_foreach_task(struct edc_session_manager *sm, edc_session_task_visit_fn cb, void *ctx)
{
    if (!sm || !cb)
        return;

    /* Fixed order (session chain x group chain x member chain): the caller (the
     * scheduler) relies on it for determinism -- the same runnable set drives in the
     * same order each round. Unloaded sessions have no groups and are skipped. */
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (!s)
            continue;
        struct edc_task_group *group;
        DL_FOREACH(s->groups, group)
        {
            struct edc_task *t;
            LL_FOREACH2(group->members, t, group_next)
            cb(ctx, t);
        }
    }
}

/* ── Locating the owner of a task ──────────────────────────────────────────
 * This used to be a uthash cache filled by parsing every unloaded session's session.json
 * (sm_task_index_*: ~80 lines). With SQLite there is nothing to cache -- the owner is one
 * primary-key lookup, and the database is always current, whereas the old index had to be
 * invalidated by hand after every save. Deleted rather than kept alongside, per the
 * design doc §6.2. */

void edc_session_manager_reset_task_index(struct edc_session_manager *sm)
{
    /* Kept as a no-op so call sites (created/saved/loaded) do not all have to change this
     * round; there is no index left to invalidate. Stage 4 removes it. */
    (void)sm;
}

struct edc_session *edc_session_manager_session_for_task(struct edc_session_manager *sm, int64_t task_id)
{
    if (!sm || task_id <= 0)
        return NULL;

    /* An in-memory session wins: it is authoritative and may hold a task that has not
     * been flushed yet (nothing wrote this run). */
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (!s)
            continue;
        if (edc_session_find_task(s, task_id))
            return s;
    }

    /* Otherwise ask the store: one primary-key lookup. */
    char *owner = edc_state_store_session_for_task(task_id);
    if (!owner)
        return NULL;
    struct edc_session *found = edc_session_manager_find(sm, owner);
    free(owner);
    return found;
}

/* ── Operations ────────────────────────────────────────────── */

struct edc_session *edc_session_manager_create(struct edc_session_manager *sm, const char *name)
{
    return edc_session_manager_create_ex(sm, name, NULL);
}

struct edc_session *edc_session_manager_create_ex(struct edc_session_manager *sm, const char *name, const char *workdir)
{
    if (!sm)
        return NULL;

    sds sname = (name && strlen(name) > 0) ? sdsnew(name) : edc_sm_gen_session_name(sm);

    /* The row comes first: that is what allocates the id. */
    /* Allocate an id, then check that its directory is FREE.
     *
     * /session remove takes the session out of the index and the database but leaves its
     * directory behind (it holds chat.jsonl, which the TUI reads). A fresh id landing on
     * such a directory would hand a new session the removed one's transcript. The old
     * design avoided this with a <ws>/.session_seq high-water mark plus a directory scan;
     * the database allocates now, so the check happens here instead: take an id, and if
     * "s<id>" already exists, drop the row and take the next one. AUTOINCREMENT never
     * reuses a number, so the loop always terminates at a free directory.
     */
    int64_t id = 0;
    bool allocated = false;
    for (int attempt = 0; attempt < 1024 && !allocated; attempt++)
    {
        if (!edc_state_store_insert_session_new(sname, &id))
        {
            log_error("session create: the store could not allocate a session id");
            sdsfree(sname);
            return NULL;
        }
        sds dir = edc_session_dir_name(id);
        sds path = edc_path_join(sm->config->workspace, dir, NULL);
        const bool taken = edc_file_exists(path);
        sdsfree(path);
        sdsfree(dir);
        if (!taken)
        {
            allocated = true;
            break;
        }
        /* Taken by a removed session's leftovers: give this id back and ask for the next. */
        edc_state_store_delete_session(id);
        log_info("session create: id %" PRId64 " is taken by an existing directory, trying the next",
                 (long long)id);
    }
    if (!allocated)
    {
        log_error("session create: could not find a free session id");
        sdsfree(sname);
        return NULL;
    }

    struct edc_session *s = edc_session_new_with_workdir(sm->config, id, sname, workdir);
    sdsfree(sname);

    if (!s)
        return NULL;

    /* Add to array */
    int new_count = sm->session_count + 1;
    struct edc_session **new_arr = g_new0(struct edc_session *, new_count);
    if (sm->sessions && sm->session_count > 0)
        memcpy(new_arr, sm->sessions, (size_t)sm->session_count * sizeof(struct edc_session *));
    new_arr[sm->session_count] = s;
    free(sm->sessions);
    sm->sessions = new_arr;
    sm->session_count = new_count;
    sm->active_index = sm->session_count - 1;

    s->last_active = (uint64_t)edc_datetime_now();

    /* One row, not a rewritten list: creation is a database write now. */
    edc_state_store_ensure_session(s->session_id, s->name, s->created_at);
    edc_sm_persist_active(sm);
    log_info("created session: id=%s name=%s", edc_id_str(s->session_id), s->name);
    return s;
}

bool edc_session_manager_switch(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return false;

    int idx = edc_session_manager_index_of(sm, name_or_id);
    if (idx < 0)
    {
        log_error("session not found: %s", name_or_id);
        return false;
    }

    if (idx == sm->active_index)
    {
        log_info("already on session: %s", name_or_id);
        return true;
    }

    /* Save the outgoing session's history. Its *state* is persisted by the
     * caller before switching (see edc_commands_handle_session_switch) — the
     * manager has no scheduler handle, so saving state here would drop the
     * foreground group id and a new task would later spawn its own group. */
    struct edc_session *old = edc_session_manager_get_active(sm);
    if (old)
    {
        old->last_active = (uint64_t)edc_datetime_now();
    }

    /* Switch */
    sm->active_index = idx;
    struct edc_session *new_sess = sm->sessions[idx];

    new_sess->last_active = (uint64_t)edc_datetime_now();

    edc_sm_persist_active(sm);
    log_info("switched to session: id=%s name=%s", edc_id_str(new_sess->session_id), new_sess->name);
    return true;
}

bool edc_session_manager_stop(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return false;
    if (sm->session_count <= 1)
    {
        log_error("cannot stop the last session");
        return false;
    }

    int idx = edc_session_manager_index_of(sm, name_or_id);
    if (idx < 0)
    {
        log_error("session not found: %s", name_or_id);
        return false;
    }

    struct edc_session *to_stop = sm->sessions[idx];

    /* Save history one final time — but only when this run actually loaded the
     * session's state. For an id/name-only shell (never activated in this run)
     * the file on disk is the newer copy, and writing the shell would wipe it. */
    if (to_stop->state_loaded)
        edc_session_save_state_tagged(to_stop, NULL, "sm:stop");

    /* Remove from array */
    int new_count = sm->session_count - 1;
    struct edc_session **new_arr = g_new0(struct edc_session *, new_count);
    int write = 0;
    for (int i = 0; i < sm->session_count; i++)
    {
        if (i != idx)
            new_arr[write++] = sm->sessions[i];
    }
    free(sm->sessions);
    sm->sessions = new_arr;
    sm->session_count = new_count;

    /* Adjust active_index */
    if (sm->active_index >= idx)
        sm->active_index--;
    if (sm->active_index < 0)
        sm->active_index = 0;
    if (sm->active_index >= sm->session_count)
        sm->active_index = sm->session_count - 1;

    log_info("stopped session: id=%s name=%s", edc_id_str(to_stop->session_id), to_stop->name);
    edc_session_free(to_stop);

    /* Update active session's last_active */
    struct edc_session *active = edc_session_manager_get_active(sm);
    if (active)
    {
        active->last_active = (uint64_t)edc_datetime_now();
    }

    edc_sm_persist_active(sm);
    return true;
}

bool edc_session_manager_rename(struct edc_session_manager *sm, const char *name_or_id, const char *new_name)
{
    if (!sm || !name_or_id || !new_name || strlen(new_name) == 0)
        return false;

    struct edc_session *s = edc_session_manager_find(sm, name_or_id);
    if (!s)
    {
        log_error("session not found: %s", name_or_id);
        return false;
    }

    sdsfree(s->name);
    s->name = sdsnew(new_name);

    edc_state_store_ensure_session(s->session_id, s->name, s->created_at);
    log_info("renamed session %s to %s", edc_id_str(s->session_id), new_name);
    return true;
}

sds edc_session_manager_list(struct edc_session_manager *sm)
{
    if (!sm)
        return sdsnew("**Sessions** — none.");

    sds result = sdscatprintf(sdsempty(), "## Sessions (%d)\n\n", sm->session_count);

    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        size_t msg_count = edc_session_get_message_count(s);
        bool active = (i == sm->active_index);

        result = sdscatprintf(result, "- `[%s]` **%s**%s\n", edc_id_str(s->session_id),
                              s->name ? s->name : "(unnamed)",
                              active ? " · **current**" : "");
        {
            /* Formatted here, at the presentation edge: the value itself is an integer. */
            sds created = s->created_at ? edc_datetime_format((time_t)s->created_at) : NULL;
            result = sdscatprintf(result, "  - messages: %zu · created: %s\n", msg_count,
                                  created ? created : "unknown");
            sdsfree(created);
        }
    }
    if (sm->session_count == 0)
        result = sdscat(result, "_No sessions._\n");

    return result;
}

/* ── Persistence ───────────────────────────────────────────── */

void edc_session_manager_save_all(struct edc_session_manager *sm)
{
    if (!sm)
        return;

    /* Session state is not written here: this function has no scheduler, and since the
     * move to the store every per-session save needs one. The paths that want the tree on
     * disk are edc_session_store_save_all_sessions_state() and
     * edc_session_store_save_session_state_tagged(), and both call
     * edc_session_manager_save_loaded_states() after this.
     *
     * What is left is the one fact this layer owns: which session is active. */
    edc_sm_persist_active(sm);
}

void edc_session_manager_save_loaded_states(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sm)
        return;

    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        /* An unloaded session is the id/name-only shell built from
         * sessions.json: this run never read its groups/tasks/workdir back from
         * disk, so the file is the newer copy. Saving the shell here would wipe
         * the real state — and with it the bound working directory, which is
         * exactly how a session "loses" its workdir after a restart. */
        if (!s || !s->state_loaded)
            continue;
        edc_session_save_state_tagged(s, sched, "sm:save-loaded");
    }
}
