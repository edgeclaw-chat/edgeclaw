#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_compat.h"
#include "edc_path.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_datetime_utils.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_state_serializer.h"
#include "edc_state_probe.h"
#include "edc_state_store.h"
#include "edc_string_utils.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct edc_session *edc_session_new(struct edc_config *config, int64_t session_id, const char *name)
{
    return edc_session_new_with_workdir(config, session_id, name, NULL);
}

const char *edc_session_get_workdir(struct edc_session *session)
{
    if (!session)
        return NULL;
    return session->workdir;
}

bool edc_session_set_workdir(struct edc_session *session, const char *workdir, EdcError **error)
{
    if (!session)
        return false;

    /* NULL/empty unbinds: back to the launch directory. */
    if (!workdir || !workdir[0])
    {
        if (session->workdir)
        {
            sdsfree(session->workdir);
            session->workdir = NULL;
        }
        return true;
    }

    /* A relative |workdir| resolves against the process cwd here (callers that
     * want a different anchor pre-resolve with edc_session_resolve_workdir). */
    sds resolved = edc_session_resolve_workdir(NULL, workdir, error);
    if (!resolved)
        return false;

    /* The bound directory must be writable inside the sandbox, same as the
     * launch directory: grant it before storing it, so tool execution there
     * cannot be denied by path validation or the kernel sandbox. */
    edc_config_add_sandbox_rw(session->config, resolved);

    if (session->workdir)
        sdsfree(session->workdir);
    session->workdir = sdsnew(resolved);
    sdsfree(resolved);

    log_info("session %s bound to working directory %s", edc_id_str(session->session_id), session->workdir);
    return true;
}

sds edc_session_resolve_workdir(const char *base, const char *workdir, EdcError **error)
{
    if (!workdir || !workdir[0])
    {
        if (error)
            *error = edc_error_new(EDC_ERR_INVAL, "empty working directory");
        return NULL;
    }

    /* Absolute paths are used as-is; a leading "~" expands to the home
     * directory; any other relative path is anchored at |base| (a session's
     * current working directory), or the process cwd when none. */
    sds candidate;
    if (workdir[0] == '~' && (workdir[1] == '/' || workdir[1] == '\0'))
    {
        sds home = edc_homedir();
        candidate = workdir[1] ? edc_path_join(home, workdir + 2, NULL) : sdsnew(home);
        sdsfree(home);
    }
    else if (edc_path_is_absolute(workdir))
    {
        candidate = sdsnew(workdir);
    }
    else
    {
        sds anchor;
        if (base && base[0])
        {
            anchor = sdsnew(base);
        }
        else
        {
            anchor = edc_cwd();
        }
        candidate = edc_path_join(anchor, workdir, NULL);
        sdsfree(anchor);
    }

    char *resolved = realpath(candidate, NULL);
    if (!resolved)
    {
        if (error)
            *error = edc_error_new(EDC_ERR_INVAL, "working directory does not exist: %s", candidate);
        sdsfree(candidate);
        return NULL;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        if (error)
            *error = edc_error_new(EDC_ERR_INVAL, "not a directory: %s", candidate);
        free(resolved);
        sdsfree(candidate);
        return NULL;
    }

    /* Normal form at the boundary: everything downstream (the tools' path anchoring,
     * the sandbox lists, the prompt) then sees one spelling of the same directory --
     * no trailing separator, no mixed ones. */
    sds out = edc_path_norm(resolved);
    free(resolved);
    sdsfree(candidate);
    return out ? out : sdsnew(".");
}

sds edc_session_dir_name(int64_t session_id)
{
    /* An sds copy of the id's text form. edc_id_str() returns a rotating buffer, so it
     * cannot be stored; this is the storing variant, and it spells the id exactly the same
     * way -- one representation, no prefix. */
    return sdscatprintf(sdsempty(), "%lld", (long long)session_id);
}

struct edc_session *edc_session_new_with_workdir(struct edc_config *config, int64_t session_id, const char *name,
                                                 const char *workdir)
{
    struct edc_session *session = g_new0(struct edc_session, 1);
    session->session_id = session_id;
    {
        /* The name falls back to the id, rendered the one way an id is rendered. */
        session->name = sdsnew(name && strlen(name) > 0 ? name : edc_id_str(session_id));
    }
    session->created_at = (uint64_t)edc_datetime_now();
    session->last_active = session->created_at;
    session->config = config;
    {
        sds dir = edc_session_dir_name(session_id);
        session->history_dir = edc_path_join(config->workspace, dir, NULL);
        sdsfree(dir);
    }
    session->next_completion_seq = 1;
    session->memo = edc_session_memory_new();

    if (workdir && workdir[0])
    {
        /* An explicitly requested working directory is a precondition, not a
         * hint: if it cannot be resolved to an existing directory, fail the
         * creation rather than silently falling back to the launch directory. */
        EdcError *error = NULL;
        if (!edc_session_set_workdir(session, workdir, &error))
        {
            log_warn("session %s: cannot bind working directory '%s': %s", edc_id_str(session->session_id),
                     workdir, error ? error->message : "unknown");
            edc_error_free(error);
            edc_session_free(session);
            return NULL;
        }
    }

    return session;
}

void edc_session_free(struct edc_session *session)
{
    if (!session)
        return;

    /* Groups (and the tasks they own) belong to the session now — free them
     * here so every session's groups are released, not just the one the
     * scheduler happened to be bound to. */
    struct edc_task_group *group, *gtmp;
    DL_FOREACH_SAFE(session->groups, group, gtmp)
    {
        DL_DELETE(session->groups, group);
        edc_task_group_free(group);
    }
    session->groups = NULL;

    edc_session_memory_free(session->memo);
    sdsfree(session->name);
    sdsfree(session->history_dir);
    sdsfree(session->workdir);
    sdsfree(session->last_local_cmd);
    free(session);
}

size_t edc_session_get_message_count(struct edc_session *session)
{
    /* Messages belong to turns: sum every turn's message count (in-flight active_turn included). */
    if (!session)
        return 0;
    size_t n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            for (size_t i = t->turns.head; i < t->turns.tail; i++)
            {
                struct edc_turn *turn = (struct edc_turn *)t->turns.data[i];
                if (turn)
                    n += (size_t)turn->msg_count;
            }
            if (t->active_turn)
                n += (size_t)t->active_turn->msg_count;
        }
    }
    return n;
}

/* ══════════════════════════════════════════════════════════════
   Task/Group management
   ══════════════════════════════════════════════════════════════ */

void edc_session_add_group(struct edc_session *session, struct edc_task_group *group)
{
    if (!session || !group)
        return;
    /* Owner back-pointer: when a task finishes in a background session, the
     * scheduler uses it to write the turn/notice/persist back to THAT session.
     * Core has no "current session" copy to fall back on, so this pointer is the
     * only source of ownership. */
    group->session = session;
    DL_APPEND(session->groups, group);
}

void edc_session_remove_group(struct edc_session *session, struct edc_task_group *group)
{
    if (!session || !group)
        return;
    DL_DELETE(session->groups, group);
    group->session = NULL;
}

struct edc_task_group *edc_session_find_group(struct edc_session *session, int64_t group_id)
{
    if (!session || !group_id)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        if (group->group_id == group_id)
            return group;
    }
    return NULL;
}

/* Container of system tasks; see edc_session.h (the only core-side read of kind). */
struct edc_task_group *edc_session_find_system_group(struct edc_session *session)
{
    if (!session)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        if (edc_task_group_is_system(group))
            return group;
    }
    return NULL;
}

struct edc_task *edc_session_find_task(struct edc_session *session, int64_t task_id)
{
    if (!session || !task_id)
        return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        struct edc_task *t = edc_task_group_find_task(group, task_id);
        if (t)
            return t;
    }
    return NULL;
}

/* ── system task: carries ownerless / session-level messages ─ */

void edc_session_set_active_task(struct edc_session *session, struct edc_task *task)
{
    if (!session)
        return;
    session->active_task = task;
}

struct edc_task *edc_session_get_active_task(struct edc_session *session)
{
    return session ? session->active_task : NULL;
}

sds edc_session_status_text(struct edc_session *session, bool is_current, const char *foreground_group_id)
{
    if (!session)
        return sdsnew("[session] no session");

    /* Tally groups and per-state tasks. */
    int group_count = 0, orphan_count = 0, task_count = 0;
    /* The histogram buckets by DISPLAY NAME: /task status's wording is existing
     * external behaviour and this table freezes the vocabulary (order = the old enum
     * order, output byte-identical). The set is closed, so a miss can only be a
     * missed state-machine edit -- then warn, do not silently drop.
     * WAITING_INPUT is retired (old name = READY), always 0, kept only for order. */
    static const char *const k_state_order[] = {"IDLE",        "WAITING_DEPS", "WAITING_INPUT", "LLM_PROCESSING",
                                                "TOOL_CALLING", "COMPLETED",     "CANCELLED",     "ERROR"};
    enum
    {
        k_state_order_n = sizeof(k_state_order) / sizeof(k_state_order[0])
    };
    int state_counts[k_state_order_n] = {0};
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        group_count++;
        if (g->is_orphaned)
            orphan_count++;
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            task_count++;
            const char *state = edc_task_state_str(t);
            size_t bucket = k_state_order_n;
            for (size_t s = 0; s < k_state_order_n; s++)
            {
                if (strcmp(state, k_state_order[s]) == 0)
                {
                    bucket = s;
                    break;
                }
            }
            if (bucket == k_state_order_n)
            {
                log_warn("status: task %s reports an unknown state '%s'", edc_id_str(t->task_id), state);
                continue;
            }
            state_counts[bucket]++;
        }
    }

    sds out = sdscatprintf(sdsempty(), "## Session Status `[%s] %s`%s\n\n", edc_id_str(session->session_id),
                           edc_session_get_name(session), is_current ? " · current" : " · background");
    out = sdscatprintf(out, "- **State**: %s\n", is_current ? "current (live)" : "background");
    out = sdscatprintf(out, "- **Working Directory**: %s\n",
                       edc_session_get_workdir(session) ? edc_session_get_workdir(session) : "(default)");
    out = sdscatprintf(out, "- **Groups**: %d", group_count);
    if (orphan_count > 0)
        out = sdscatprintf(out, " (%d orphaned)", orphan_count);
    out = sdscat(out, "\n");
    out = sdscatprintf(out, "- **Tasks**: %d", task_count);

    /* Per-state breakdown, in the frozen word-list order, only for non-zero buckets. */
    bool first_state = true;
    for (size_t s = 0; s < k_state_order_n; s++)
    {
        if (state_counts[s] == 0)
            continue;
        out = sdscatprintf(out, "%s %s %d", first_state ? " —" : " ·", k_state_order[s], state_counts[s]);
        first_state = false;
    }
    out = sdscat(out, "\n");

    if (is_current)
    {
        out = sdscatprintf(out, "- **Foreground group**: %s\n", foreground_group_id);
        struct edc_task *active = session->active_task;
        if (active)
            out = sdscatprintf(out, "- **Active task**: %s%s%s\n", edc_id_str(active->task_id),
                               (active->description && *active->description) ? " — " : "",
                               active->description ? active->description : "");
        else
            out = sdscat(out, "- **Active task**: (none)\n");
    }
    else if (group_count == 0)
    {
        out = sdscat(out, "- _live task state not loaded (switch to this session to load it)_\n");
    }

    out = sdscatprintf(out, "- **Messages**: %zu\n", edc_session_get_message_count(session));
    {
        sds when = edc_datetime_format((time_t)session->last_active);
        out = sdscatprintf(out, "- **Last Active**: %s\n", when ? when : "unknown");
        sdsfree(when);
    }
    return out;
}

/* ── Full state persistence ────────────────────────────────── */

/* ── Where state actually lives now ─────────────────────────────────────────
 * These two functions are the ONLY door the rest of the kernel uses for persistence, so
 * the move to SQLite stops here: the signatures are unchanged and every call site (all
 * 18 of them) is untouched. What used to be "serialize the tree, write session.json,
 * rename" is now "one transaction against <workspace>/edgeclaw.db".
 *
 * The legacy session.json is neither read nor deleted -- this is a development build with
 * no migration path by design (design doc §3.3), so an old file is simply ignored with one
 * WARN. It is left on disk because deleting someone's history on the first run of a new
 * build is not this function's decision to make. */

/* Log the legacy-file warning at most once per session per process: save_state is called
 * at every tool boundary, and a warning per call would bury the log. */
static void warn_legacy_state_file_once(struct edc_session *session)
{
    if (!session || session->legacy_state_warned || !session->history_dir)
        return;
    sds path = edc_path_join(session->history_dir, "session.json", NULL);
    if (edc_file_exists(path))
    {
        log_warn("state_store: ignoring legacy %s -- this build keeps session state in "
                 "<workspace>/edgeclaw.db. The old file is left untouched; remove it "
                 "yourself once you no longer need it.",
                 path);
    }
    sdsfree(path);
    session->legacy_state_warned = true;
}

/* The body. The store call is the only thing that gets timed; the cost it reports back
 * (bytes appended to the WAL, rows changed) is what the probe records as "bytes written".
 * Note what is NOT here any more: a second serialization of the tree. It moved to the
 * wrapper, after the clock stopped -- see edc_session_save_state_tagged. */
static bool session_save_state_impl(struct edc_session *session, struct edc_scheduler *sched,
                                    struct edc_state_flush_cost *out_cost)
{
    if (out_cost)
    {
        out_cost->io_bytes = -1;
        out_cost->rows = -1;
    }

    if (!session || session->session_id <= 0)
        return false;

    /* A quarantined session must not persist: its in-memory form is just the shell after
     * a failed load, so writing it would overwrite a newer database copy with nothing.
     * (The quarantine flag itself is set by the loader; stage 4 replaces this with a
     * per-session `broken` column.) */
    if (session->state_quarantined)
    {
        log_warn("save_state: refusing to persist quarantined session %s",
                 edc_id_str(session->session_id));
        return false;
    }

    if (!edc_state_store_open(session->config))
        return false;

    warn_legacy_state_file_once(session);

    const bool ok = edc_state_store_flush_measured(session, sched, out_cost);

    return ok;
}

/* tag names the call site ("r8:pre_dispatch", "settle:cancelled", ...) so the
 * baseline CSV can answer "how many flushes does one tool boundary cause" --
 * the number that decides how much work the stage-2 SYNC/DEFERRED merge window
 * has to do. Unknown tags are harmless. */
bool edc_session_save_state_tagged(struct edc_session *session, struct edc_scheduler *sched,
                                   const char *tag)
{
    const bool probe = edc_state_probe_enabled();
    struct edc_state_flush_cost cost = {-1, -1};

    const uint64_t t0 = probe ? edc_state_probe_now_ns() : 0;
    const bool ok = session_save_state_impl(session, sched, probe ? &cost : NULL);
    const uint64_t t1 = probe ? edc_state_probe_now_ns() : 0;

    if (probe)
    {
        /* Everything below happens AFTER t1. The "bytes" half of the baseline needs the
         * tree serialized a second time, which costs about as much as the flush itself --
         * and charging it to dur_ms is exactly what made the first baseline read 0.32 ms
         * for a 268-byte session. Instrumentation must never be what the timer sees. */
        size_t doc_bytes = 0;
        long n_tasks = 0;
        long n_msgs = 0;

        cJSON *tree = edc_state_serialize(session, sched);
        if (tree)
        {
            char *txt = cJSON_PrintUnformatted(tree);
            if (txt)
            {
                doc_bytes = strlen(txt);
                cJSON_free(txt);
            }
            cJSON_Delete(tree);
        }

        if (session)
        {
            struct edc_task_group *g;
            DL_FOREACH(session->groups, g)
                n_tasks += g->member_count;
            n_msgs = (long)edc_session_get_message_count(session);
        }

        edc_state_probe_save(t1 - t0, doc_bytes, cost.io_bytes, cost.rows, n_tasks, n_msgs, ok,
                             session ? edc_id_str(session->session_id) : NULL, tag);
    }
    return ok;
}

/* tag names the call site ("r8:pre_dispatch", "settle:cancelled", ...) so the
 * baseline CSV can answer "how many flushes does one tool boundary cause" --
 * the number that decides how much work the stage-2 SYNC/DEFERRED merge window
 * has to do. Unknown tags are harmless. */
bool edc_session_save_state(struct edc_session *session, struct edc_scheduler *sched)
{
    return edc_session_save_state_tagged(session, sched, "direct");
}

/* Body with the measured quantities reported back: how many rows the store produced, and
 * what the loader made of them (tasks / messages) -- the "O(history)" side of the cost. */
static bool session_load_state_impl(struct edc_session *session, struct edc_scheduler *sched,
                                    long *out_rows, long *out_tasks, long *out_msgs)
{
    *out_rows = 0;
    *out_tasks = 0;
    *out_msgs = 0;

    if (!session || session->session_id <= 0)
        return false;

    if (!edc_state_store_open(session->config))
        return false;

    warn_legacy_state_file_once(session);

    /* Tell the two "nothing loaded" cases apart BEFORE loading: a session that was never
     * written may legitimately be saved as an empty tree, but a session that IS in the
     * database and failed to read must never be overwritten -- that is the v1->v2 incident
     * this guardrail exists for (a failed load followed by a persist wiped s7). */
    const bool had_rows = edc_state_store_has_session(session->session_id);

    if (!edc_state_store_load(session, sched))
    {
        if (had_rows)
        {
            session->state_quarantined = true;
            log_error("load_state: %s exists in the database but could not be read -- this "
                      "run will NOT persist it (otherwise the half-built session would "
                      "overwrite the real one)",
                      edc_id_str(session->session_id));
        }
        return false;
    }

    {
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
            *out_tasks += g->member_count;
        *out_msgs = (long)edc_session_get_message_count(session);
    }

    /* "file_bytes" used to be the size of session.json. The new backend has no single
     * file to measure, so the column now reports what the load had to produce: rows
     * (messages + tasks). It is a different unit and is named `rows` in the CSV -- keeping
     * the old name here would have invited exactly the wrong reading. */
    *out_rows = *out_msgs + *out_tasks;
    return true;
}

bool edc_session_load_state(struct edc_session *session, struct edc_scheduler *sched)
{
    const bool probe = edc_state_probe_enabled();
    const uint64_t t0 = probe ? edc_state_probe_now_ns() : 0;

    long rows = 0;
    long n_tasks = 0;
    long n_msgs = 0;
    const bool ok = session_load_state_impl(session, sched, &rows, &n_tasks, &n_msgs);
    const uint64_t t1 = probe ? edc_state_probe_now_ns() : 0;

    /* Every ATTEMPT is recorded, including the ones that found nothing: "we activated 40
     * sessions and none of them had rows" and "we never load anything" must not look the
     * same. ok=0 with rows=0 is the brand-new-session case, not a failure. */
    if (probe)
        edc_state_probe_load(t1 - t0, rows, ok, n_tasks, n_msgs,
                             session ? edc_id_str(session->session_id) : NULL, "startup");
    return ok;
}

/* ── Multi-session support ─────────────────────────────────── */

void edc_session_reset(struct edc_session *session)
{
    if (!session)
        return;

    /* Clear active task */
    session->active_task = NULL;

    /* Groups (and their tasks) belong to the session — drop them for real, not
     * just unlink the head pointer. */
    struct edc_task_group *group, *gtmp;
    DL_FOREACH_SAFE(session->groups, group, gtmp)
    {
        DL_DELETE(session->groups, group);
        edc_task_group_free(group);
    }
    session->groups = NULL;

    /* the foreground group went with groups */
    session->foreground_group = NULL;
}

const char *edc_session_get_name(struct edc_session *session)
{
    if (!session)
        return NULL;
    if (session->name)
        return session->name;
    /* No name: show the id, which is a number. */
    return edc_id_str(session->session_id);
}
