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

void edc_session_store_save_session_state_tagged(struct edc_session_manager *sm,
                                                 struct edc_scheduler *sched,
                                                 struct edc_session *session, const char *tag)
{
    if (!sm || !sched)
        return;

    /* Write the index/history (full) first, then rewrite the target session's state. */
    edc_session_manager_save_all(sm);

    struct edc_session *target = session ? session : edc_session_manager_get_active(sm);
    if (target)
        edc_session_save_state_tagged(target, sched, tag);
}

void edc_session_store_save_session_state(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                          struct edc_session *session)
{
    edc_session_store_save_session_state_tagged(sm, sched, session, "store:direct");
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
    edc_session_store_save_session_state_tagged(sm, sched, active, "session:stop");
}

/* Forget that any request was in flight: the process is going down, so a reply that is
 * still travelling must not land. That is ALL this does -- it must not touch a task's
 * state. (It used to SIGTERM every task of the active session, which marks them CANCELLED
 * and persists it, undoing the INTERRUPTED edc_scheduler_close had just written: a clean
 * exit silently cancelled the whole session instead of leaving it resumable.) */
static void drop_in_flight_request(void *ctx, struct edc_task *task)
{
    (void)ctx;
    if (!task)
        return;
    task->current_request_id = NULL;
    task->cancelled_request = 1;
}

void edc_session_store_drop_in_flight_requests(struct edc_session_manager *sm, struct edc_scheduler *sched)
{
    (void)sched;
    if (!sm)
        return;
    edc_session_manager_foreach_task(sm, drop_in_flight_request, NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Unloading an idle session
 *
 *  The in-memory tree is a CACHE of the database, not the source of truth, so it can be
 *  handed back. Before the group history became ids this would have been pointless (a
 *  loaded session held every unfolded message); now a load is cheap, which is what makes
 *  eviction worth doing at all.
 * ══════════════════════════════════════════════════════════════════════════ */

bool edc_session_has_live_work(struct edc_session *session)
{
    if (!session)
        return false;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            /* ALIVE means "may still advance on its own": queued, running, waiting on
             * tools or on a dependency. INTERRUPTED is deliberately NOT live -- it waits
             * for a decision, and that decision is already on disk. */
            if (edc_task_is_alive(t))
                return true;
        }
    }
    return false;
}

/* Does any OTHER in-memory session hold a pointer into |victim|'s tree?
 *
 * The dangerous link is a dependency edge: `target->deps` holds `dep->source` as a LIVE
 * pointer to the source task, and the edge is used long after it fires -- a queued
 * dependent's request injects `dep->source->output`. `/task new -d <id>` explicitly allows
 * a dependency to live in another session, so freeing the source's session would leave a
 * dependent holding a pointer into freed memory (and, before that, silently drop the
 * output it was supposed to carry). */
struct dep_scan_ctx
{
    struct edc_session *victim;
    bool referenced;
};

static void dep_scan_cb(void *ctx, struct edc_task *task)
{
    struct dep_scan_ctx *c = ctx;
    if (!c || c->referenced || !task)
        return;
    struct edc_task_group *g = task->group;
    if (g && g->session == c->victim)
        return; /* this task belongs to the victim; its own edges go with it */

    struct edc_task_dep *dep;
    LL_FOREACH(task->deps, dep)
    {
        /* A source inside the victim's tree means the dependent would dereference freed
         * memory. Cheap alias test: the tree is being scanned anyway. */
        if (dep->source && dep->source->group && dep->source->group->session == c->victim)
        {
            c->referenced = true;
            return;
        }
    }
}

static bool session_referenced_elsewhere(struct edc_session_manager *sm, struct edc_session *victim)
{
    struct dep_scan_ctx c = {victim, false};
    edc_session_manager_foreach_task(sm, dep_scan_cb, &c);
    return c.referenced;
}

bool edc_session_unload(struct edc_session_manager *sm, struct edc_scheduler *sched,
                        struct edc_session *session)
{
    if (!sm || !session)
        return false;

    /* Never the active one: the scheduler is driving it, and the next thing it does would
     * dereference a tree that just disappeared. */
    if (edc_session_manager_get_active(sm) == session)
        return false;

    if (!session->state_loaded)
        return false; /* already just a shell */

    /* A quarantined session must never be persisted (its in-memory form is the shell left
     * by a failed load), and its tree is all we have -- so it is never evicted either. */
    if (session->state_quarantined)
        return false;

    if (edc_session_has_live_work(session))
        return false;

    /* Another loaded session may depend on a task in this one (a cross-session `-d`). Its
     * `dep->source` is a live pointer into this tree, so neither can go: this session
     * keeps its memory, the other keeps its output injection. Cross-session dependencies
     * are rare, so the cost of this check is the cost of a scan nobody notices. */
    if (session_referenced_elsewhere(sm, session))
    {
        log_info("session_unload: %s stays resident (another session depends on its tasks)",
                 session->session_id);
        return false;
    }

    /* Persist FIRST. If this fails the tree is the only copy and dropping it would lose
     * the conversation, so the eviction is abandoned instead. */
    edc_session_store_save_session_state_tagged(sm, sched, session, "unload");

    /* Release the tree. memo/session-memory is left alone: it is a handful of entries and
     * the loader rebuilds it anyway (edc_state_serializer.c frees and reconstructs it),
     * so keeping it avoids a pointless gap in /session status. */
    edc_session_reset(session);

    /* The shell a never-activated session has, which is what the load path keys on. From
     * here edc_session_store_load_session() brings it back on demand. */
    session->state_loaded = false;

    log_info("session_unload: %s released (its state is in the database)", session->session_id);
    return true;
}

int edc_session_manager_loaded_count(struct edc_session_manager *sm)
{
    if (!sm)
        return 0;
    int n = 0;
    for (int i = 0; i < sm->session_count; i++)
        if (sm->sessions[i] && sm->sessions[i]->state_loaded)
            n++;
    return n;
}

int edc_session_manager_evict_idle(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                   int max_loaded)
{
    if (!sm)
        return 0;
    if (max_loaded <= 0)
        max_loaded = EDC_SESSION_MAX_LOADED;

    int evicted = 0;
    /* One at a time, oldest first: evicting can free a lot, so re-scanning after each is
     * cheaper than it looks and keeps the "how many are resident" count honest. */
    while (edc_session_manager_loaded_count(sm) > max_loaded)
    {
        struct edc_session *victim = NULL;
        struct edc_session *active = edc_session_manager_get_active(sm);
        for (int i = 0; i < sm->session_count; i++)
        {
            struct edc_session *s = sm->sessions[i];
            if (!s || !s->state_loaded || s == active)
                continue;
            /* Skip anything that must not be dropped; unload() would refuse anyway, but
             * skipping here keeps the victim search from spinning on the same session --
             * and lets it pick a different candidate instead of giving up. */
            if (s->state_quarantined || edc_session_has_live_work(s))
                continue;
            if (session_referenced_elsewhere(sm, s))
                continue;
            if (!victim || s->last_active < victim->last_active)
                victim = s;
        }
        if (!victim)
            break; /* everything resident is active / busy / quarantined */

        if (!edc_session_unload(sm, sched, victim))
            break; /* save failed: keep what we have rather than lose it */
        evicted++;
    }
    return evicted;
}
