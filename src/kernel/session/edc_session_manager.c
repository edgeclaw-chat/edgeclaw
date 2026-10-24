#include "edc_session_manager.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_datetime_utils.h"
#include "edc_file_utils.h"
#include "edc_session.h"
#include "edc_state_serializer.h"
#include "edc_state_store.h"
#include "log.h"
#include "sds.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <dirent.h>
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

static sds edc_sm_session_seq_path(struct edc_session_manager *sm)
{
    if (!sm || !sm->config || !sm->config->workspace || sdslen(sm->config->workspace) == 0)
        return NULL;
    return edc_path_join(sm->config->workspace, ".session_seq", NULL);
}

static void edc_sm_persist_session_seq(struct edc_session_manager *sm)
{
    sds path = edc_sm_session_seq_path(sm);
    if (!path)
        return;

    sds tmp = sdscatprintf(sdsempty(), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f && errno == ENOENT && sm->config && sm->config->workspace)
    {
        /* On a fresh environment the workspace dir may not exist yet: create it and retry. */
        mkdir(sm->config->workspace, 0755);
        f = fopen(tmp, "w");
    }
    if (!f)
    {
        log_debug("session_seq: open %s for write failed: %s", tmp, strerror(errno));
    }
    else
    {
        if (fprintf(f, "%lld\n", sm->next_session_id) < 0)
            log_debug("session_seq: write %s failed: %s", tmp, strerror(errno));
        fclose(f);
        EdcError *rerr = NULL;
        if (!edc_file_rename_replace(tmp, path, &rerr))
        {
            log_warn("session_seq: %s", rerr ? rerr->message : "rename failed");
            if (rerr)
                edc_error_free(rerr);
        }
    }
    sdsfree(tmp);
    sdsfree(path);
}

static void edc_sm_load_session_seq(struct edc_session_manager *sm)
{
    sds path = edc_sm_session_seq_path(sm);
    if (!path)
        return;

    FILE *f = fopen(path, "r");
    sdsfree(path);
    if (!f)
        return; /* first run / missing file: keep the in-memory initial value */

    long long v = 0;
    if (fscanf(f, "%lld", &v) == 1 && v > sm->next_session_id)
        sm->next_session_id = v;
    fclose(f);
}

/* Observe a known number (an id in the index, or a leftover session dir name on
 * disk) into the high-water mark. Only the decimal tail of "s<N>" / "sess-<N>" is
 * recognized, matching the issuing format. */
static void edc_sm_observe_session_id(struct edc_session_manager *sm, const char *id)
{
    if (!sm || !id || !*id)
        return;

    const char *num = NULL;
    if (strncmp(id, "sess-", 5) == 0)
        num = id + 5;
    else if (id[0] == 's')
        num = id + 1;
    if (!num || num[0] < '0' || num[0] > '9')
        return;

    char *end = NULL;
    long long n = strtoll(num, &end, 10);
    if (end == num || (end && *end != '\0'))
        return;

    if (n >= sm->next_session_id)
    {
        sm->next_session_id = n + 1;
        edc_sm_persist_session_seq(sm);
    }
}

/* Also observe existing session dirs under workspace: /session remove only takes a
 * session out of the index and leaves the directory, so a max-number session deleted
 * before this change is "not in the index, directory still there". */
static void edc_sm_observe_session_dirs(struct edc_session_manager *sm)
{
    if (!sm || !sm->config || !sm->config->workspace || sdslen(sm->config->workspace) == 0)
        return;

    DIR *d = opendir(sm->config->workspace);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        edc_sm_observe_session_id(sm, e->d_name);
    closedir(d);
}

static sds edc_sm_gen_session_id(struct edc_session_manager *sm)
{
    long long id = sm->next_session_id > 0 ? sm->next_session_id : 1;
    sm->next_session_id = id + 1;
    /* Persist the high-water mark on allocation: even if the session is later
     * /session remove'd, the same number is not issued again after a restart. */
    edc_sm_persist_session_seq(sm);
    return sdscatprintf(sdsempty(), "s%lld", id);
}

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
    if (!s || !s->session_id)
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
        struct edc_session *s = edc_session_new(sm->config, rows[i].session_id,
                                                rows[i].name && *rows[i].name ? rows[i].name
                                                                              : rows[i].session_id);
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

    /* Every id this run may allocate must exceed every id already used. */
    for (int i = 0; i < n; i++)
        edc_sm_observe_session_id(sm, sm->sessions[i]->session_id);

    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────── */

struct edc_session_manager *edc_session_manager_new(struct edc_config *config)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->config = config;
    sm->active_index = -1;
    sm->sessions_dir = sdsnew(config->workspace);
    sm->next_session_id = 1;

    /* Restore the previous run's high-water mark, load the index (which also feeds
     * the numbers it lists), then observe leftover session dirs -- the order ensures
     * next_session_id exceeds every used number before any create. */
    edc_sm_load_session_seq(sm);
    edc_sm_observe_session_dirs(sm);

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

struct edc_session *edc_session_manager_find(struct edc_session_manager *sm, const char *name_or_id)
{
    if (!sm || !name_or_id)
        return NULL;
    for (int i = 0; i < sm->session_count; i++)
    {
        struct edc_session *s = sm->sessions[i];
        if (strcmp(s->session_id, name_or_id) == 0)
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
        if (strcmp(s->session_id, name_or_id) == 0)
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

/* ── task id index (unloaded sessions) ───────────────────────
 * When looking up a task id across sessions and the owner is unloaded, all we need
 * is "which session it is in". There is no tasks.idx sidecar (single source of truth
 * = session.json): the first lookup parses each unloaded session's session.json once
 * into a uthash, then locates in O(1).
 *
 * Invariant: an unloaded session is never modified during the whole run (changing it
 * requires loading it first), so this table always equals "the disk-frozen state of
 * unloaded sessions"; loaded sessions are covered by the in-memory lookup and new
 * tasks are always in a loaded session, so no write-side sync is needed. */

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

    sds id = edc_sm_gen_session_id(sm);
    sds sname = (name && strlen(name) > 0) ? sdsnew(name) : edc_sm_gen_session_name(sm);

    struct edc_session *s = edc_session_new_with_workdir(sm->config, id, sname, workdir);
    sdsfree(id);
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
    edc_state_store_ensure_session(s->session_id, s->name ? s->name : s->session_id, s->created_at);
    edc_sm_persist_active(sm);
    log_info("created session: id=%s name=%s", s->session_id, s->name);
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
    log_info("switched to session: id=%s name=%s", new_sess->session_id, new_sess->name);
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

    log_info("stopped session: id=%s name=%s", to_stop->session_id, to_stop->name);
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
    log_info("renamed session %s to %s", s->session_id, new_name);
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

        result = sdscatprintf(result, "- `[%s]` **%s**%s\n", s->session_id, s->name ? s->name : "(unnamed)",
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
