#ifndef __EDC_SESSION_STORE_H__
#define __EDC_SESSION_STORE_H__

#include <stdbool.h>

/* ── session store readers and lifetime ────────────────────────
 *
 * Every operation here needs two things: the STORE itself (`session_manager`: the
 * home of sessions and tasks, the sole authority for active_index) and task
 * ORCHESTRATION (`scheduler`: task graph, signals, persist requests). So they take
 * both explicitly -- no umbrella object carries them (signature = reader roster).
 *
 * Publishing/on-screen output (channel vocabulary) and CLI history (frontend) do
 * NOT belong here. */

struct edc_scheduler;
struct edc_session;
struct edc_session_manager;
struct edc_task;

/** Resolve a task id across sessions, activating its owning session on demand.
 *
 *  Task ids are globally unique (see edc_session_manager_find_task_any), so a
 *  hit is unambiguous. Unlike the plain in-memory lookup, this also handles the
 *  case where the owning session exists on disk but has not been activated this
 *  run: it probes the unloaded sessions' session.json, and on a match loads that
 *  session (lazy activation) before retrying.
 *
 *  Returns NULL when the id is unknown on disk either (callers then fail the
 *  command — no fallback). |owner_out| (optional) receives the owning session,
 *  or NULL on miss. Loading is side-effect-free with respect to the active
 *  session: the foreground group lives on each session, not on the scheduler. */
extern struct edc_task *edc_session_store_resolve_task(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                                       const char *task_id, struct edc_session **owner_out);

/** Load a session's persisted state and reconcile any cross-session dependency
 *  edges it revealed. Marks the session loaded (memory is authoritative even for
 *  a brand-new session with no state file). This is the single entry point for
 *  activating a session; call it from every load site. */
extern bool edc_session_store_load_session(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                           struct edc_session *session);

/** Resolve and wire pending cross-session dependency edges (recorded during
 *  deserialize when a dep's source was not in the loaded session). Each pass may
 *  lazy-load another session, revealing more pending edges; the loop runs to a
 *  fixpoint. Entries that stay unresolved hold their target task (resume gate).
 *  Safe to call repeatedly; re-entrant calls are ignored (see sched->reconciling). */
extern void edc_session_store_reconcile_pending_deps(struct edc_session_manager *sm, struct edc_scheduler *sched);

/** Session label -> session object (the store reader; also the shell/command
 *  layer's way to resolve which session an input belongs to).
 *
 *  - non-empty label: look up by id/name; NOT FOUND returns NULL (the caller
 *    rejects the input -- never fall back to the active session, which was the old
 *    bug's source);
 *  - empty/NULL label: return the active session (inputs with no session notion,
 *    e.g. crontab). */
extern struct edc_session *edc_session_store_resolve_session(struct edc_session_manager *sm, const char *session_id);

/** Shutdown persist: full history + every LOADED session's state (background
 *  sessions included). Sessions never activated this run are skipped (writing an
 *  empty shell would overwrite newer state on disk). */
extern void edc_session_store_save_all_sessions_state(struct edc_session_manager *sm, struct edc_scheduler *sched);

/** Persist one session (NULL = the active one): write the sessions index (full
 *  history) first, then rewrite the target session's state.
 *
 *  |session| is an EXPLICIT parameter: each call site knows whom it is saving
 *  (a task finishing saves its owner -- a background session's task must persist
 *  even when another session is displayed; an explicit cancel saves the cancelled
 *  one). There is no "current session" to default to, so NULL is only for call
 *  sites that genuinely did not specify.
 *
 *  Call sites are all inside kernel. It used to be the assembly root's injected
 *  `save_session` port; once the store moved into kernel the port became an
 *  intra-package hop and was retired (2026-09-16). */
extern void edc_session_store_save_session_state(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                                struct edc_session *session);

/** As above, but records |tag| as the flush's call site in the stage-0 baseline
 *  probe (EDGECLAW_STATE_PROBE). Identical behaviour; tag is diagnostic only. */
extern void edc_session_store_save_session_state_tagged(struct edc_session_manager *sm,
                                                       struct edc_scheduler *sched,
                                                       struct edc_session *session,
                                                       const char *tag);

/* ── Unloading: the in-memory tree is a cache, so it can be given back ────────
 *
 * A loaded session keeps its groups/tasks in memory because the scheduler needs them. A
 * session nobody has switched to in a while needs nothing -- and since reloading is cheap
 * now (the group history is ids; a load collects them instead of materializing 40k
 * messages), holding every session that was ever activated is pure waste.
 *
 * How many stay resident: 4 is a judgement call, not a measurement. A session tree is
 * metadata (a few KB per task plus one pointer per message), so four of them stay well
 * under a megabyte even for long conversations. Raising it buys nothing measurable;
 * lowering it to 1 would make every switch reload. */
#define EDC_SESSION_MAX_LOADED 4

/* This drops one session's tree after making sure the database has it:
 *
 *   save -> reset -> state_loaded = false
 *
 * Returns false and changes NOTHING when it would be unsafe:
 *   - the session is the active one (the scheduler is driving it),
 *   - it holds work that is not finished (a task whose life is still ALIVE),
 *   - it is quarantined (a failed load must never be persisted, see state_quarantined),
 *   - the save failed (keeping the tree beats losing it).
 *
 * After this, the session is the same id/name shell it was before its first activation,
 * and the ordinary load path brings it back on demand
 * (edc_session_store_resolve_task / the /session switch handler). */
bool edc_session_unload(struct edc_session_manager *sm, struct edc_scheduler *sched,
                        struct edc_session *session);

/* Does this session hold work that has not finished? True when any member task is still
 * ALIVE (queued, running, waiting on tools or on a dependency). An INTERRUPTED task does
 * not count: it waits for a decision that survives on disk. */
bool edc_session_has_live_work(struct edc_session *session);

/* Evict idle sessions until at most |max_loaded| are resident. The active session and any
 * session with live work or a quarantine are never evicted, so the real number can exceed
 * |max_loaded|; |max_loaded| <= 0 uses EDC_SESSION_MAX_LOADED.
 *
 * Chooses the least recently active first (session->last_active). Called after a switch,
 * which is the only moment the set can grow. Returns how many were dropped. */
int edc_session_manager_evict_idle(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                   int max_loaded);

/* How many sessions are resident right now (state_loaded), for logs and tests. */
int edc_session_manager_loaded_count(struct edc_session_manager *sm);

/** Cancel all non-terminal turns of the session (NULL = active) and persist at
 *  once, so CANCELLED survives a restart (`/session stop`). */
extern void edc_session_store_stop_all_turns(struct edc_session_manager *sm, struct edc_scheduler *sched);

/** Forget that any request was in flight (shutdown path): the state was already saved
 *  by edc_scheduler_close and a reply still travelling must not land. Changes no task's
 *  state -- SIGTERM here would cancel the session and persist it. */
extern void edc_session_store_drop_in_flight_requests(struct edc_session_manager *sm, struct edc_scheduler *sched);

#endif
