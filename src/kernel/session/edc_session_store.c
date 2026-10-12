#include "edc_session_store.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <stdlib.h>
#include <string.h>

/* ── Cross-session task resolution (S2) ──────────────────────
 * A task id is globally unique (edc_session_manager_find_task_any), so a hit is
 * unambiguous. The in-memory lookup covers only sessions activated this run; when a
 * task belongs to a disk-only, not-yet-activated session, the session_manager index
 * (task_id -> session_id, parsed once from each unloaded session.json) locates the
 * owner, which is then loaded on demand. */
struct edc_task *edc_session_store_resolve_task(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                                const char *task_id, struct edc_session **owner_out)
{
    if (owner_out)
        *owner_out = NULL;
    if (!sm || !task_id)
        return NULL;

    /* 1. Memory first: look directly in the sessions activated this run. */
    struct edc_session *owner = NULL;
    struct edc_task *t = edc_session_manager_find_task_any(sm, task_id, &owner);
    if (t)
    {
        if (owner_out)
            *owner_out = owner;
        return t;
    }

    /* 2. Miss: use the session_manager index to locate the owning session (parsing
     *    each unloaded session.json once); on a hit, load it on demand. */
    struct edc_session *s = edc_session_manager_session_for_task(sm, task_id);
    if (!s)
        return NULL;
    if (!s->state_loaded)
    {
        /* Activate the owner session (also reconciles any cross-session deps it
         * reveals). Loading is side-effect-free for the active session. */
        edc_session_store_load_session(sm, sched, s);
    }

    t = edc_session_manager_find_task_any(sm, task_id, &owner);
    if (owner_out)
        *owner_out = owner;
    return t;
}

/* ── Unresolved-dependency reconciliation (S3) ────────────── */

/* Re-apply a persisted dep edge's flags once the edge is wired late. Mirrors the
 * logic in deser_deps for the local path. */
static void apply_dep_flags(struct edc_task *target, struct edc_task *source, bool completed,
                                      bool cancelled, bool injected)
{
    if (completed)
        edc_task_on_dep_completed(target, source);
    if (cancelled)
        edc_task_on_dep_cancelled(target, source);
    if (injected)
    {
        struct edc_task_dep *dep;
        LL_FOREACH(target->deps, dep)
        {
            if (dep->source == source)
            {
                dep->injected = true;
                break;
            }
        }
    }
}

static void free_pending_dep(struct edc_pending_dep *pd)
{
    sdsfree(pd->target_task_id);
    sdsfree(pd->source_task_id);
    free(pd);
}

bool edc_session_store_load_session(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                    struct edc_session *session)
{
    if (!session)
        return false;
    bool ok = edc_session_load_state(session, sched);
    /* Memory is authoritative: even with no archive (a brand-new session) mark it
     * loaded, so it is never reloaded. */
    session->state_loaded = true;
    if (!sched || !sched->reconciling)
        edc_session_store_reconcile_pending_deps(sm, sched);
    return ok;
}

void edc_session_store_reconcile_pending_deps(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sm || !sched)
        return;
    if (sched->reconciling)
        return; /* re-entrant call from resolve_task: let the outer loop drain */
    sched->reconciling = true;

    /* Each pass resolves what it can; loading an owner session may reveal more
     * pending edges, so loop until a pass makes no progress. */
    bool progress = true;
    while (progress)
    {
        progress = false;
        struct edc_pending_dep **link = &sched->pending_deps;
        while (*link)
        {
            struct edc_pending_dep *pd = *link;

            struct edc_task *target = edc_session_manager_find_task_any(sm, pd->target_task_id, NULL);
            if (!target)
            {
                /* Dependent task is gone (e.g. its session was removed). */
                *link = pd->next;
                free_pending_dep(pd);
                progress = true;
                continue;
            }

            struct edc_task *source = edc_session_store_resolve_task(sm, sched, pd->source_task_id, NULL);
            if (source)
            {
                /* add_dependency returns false for an already-wired edge or a
                 * cycle; either way there is nothing more to do for this entry. */
                if (edc_task_add_dependency(target, source, pd->type))
                    apply_dep_flags(target, source, pd->completed, pd->cancelled, pd->injected);
                *link = pd->next;
                free_pending_dep(pd);
                progress = true;
                continue;
            }

            /* Unresolved. A completed/cancelled edge was already satisfied when
             * saved, so dropping it cannot cause a premature run. Anything else
             * must HOLD the target (resume gate) — never run with a missing dep. */
            if (pd->completed || pd->cancelled)
            {
                *link = pd->next;
                free_pending_dep(pd);
                progress = true;
                continue;
            }

            link = &pd->next;
        }
    }

    /* Whatever is left is genuinely unresolvable: surface it, and rely on the
     * resume gate to keep the target from running. */
    for (struct edc_pending_dep *pd = sched->pending_deps; pd; pd = pd->next)
        log_warn("cross-session dep unresolved: task `%s` waits on `%s` (owner session not found)", pd->target_task_id,
                 pd->source_task_id);

    sched->reconciling = false;
}

/* ── Session label -> session object (reader) ─────────────── */
struct edc_session *edc_session_store_resolve_session(struct edc_session_manager *sm, const char *session_id)
{
    if (!sm)
        return NULL;
    if (session_id && *session_id)
        return edc_session_manager_find(sm, session_id);
    return edc_session_manager_get_active(sm);
}

/* ── Session lifecycle (signals + persistence) ────────────── */

/* Shutdown-persist helper: full history + every LOADED session's state (background
 * sessions included). The foreground group is saved with the session
 * (session->foreground_group), so per-session save_state does not cross-contaminate.
 * Sessions never activated this run are skipped -- their in-memory object is only an
 * id/name shell built from sessions.json, and writing it would overwrite newer state
 * on disk (including the bound working directory); see
 * edc_session_manager_save_loaded_states. */
void edc_session_store_save_all_sessions_state(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sm || !sched)
        return;
    edc_session_manager_save_all(sm);
    edc_session_manager_save_loaded_states(sm, sched);
}

void edc_session_store_save_session_state(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                          struct edc_session *session)
{
    if (!sm || !sched)
        return;

    /* Write the index/history (full) first, then rewrite the target session's state. */
    edc_session_manager_save_all(sm);

    struct edc_session *target = session ? session : edc_session_manager_get_active(sm);
    if (target)
        edc_session_save_state(target, sched);
}

/* User /stop (/session stop): cancel all non-terminal turns of the current session
 * and persist at once, so CANCELLED survives a restart.
 * Note: when the target is not the active session (e.g. /session stop <other>), it
 * goes through edc_scheduler_send_signal_to_session(sched, target, sig, true) -- the
 * target is given explicitly by the caller. */
void edc_session_store_stop_all_turns(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sched)
        return;
    struct edc_session *active = edc_session_manager_get_active(sm);
    edc_scheduler_send_signal_to_all(sched, active, TASK_SIGTERM);
    edc_session_store_save_session_state(sm, sched, active);
}

void edc_session_store_remove_all_turns(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    if (!sched)
        return;
    edc_scheduler_send_signal_to_all(sched, edc_session_manager_get_active(sm), TASK_SIGTERM);
}
