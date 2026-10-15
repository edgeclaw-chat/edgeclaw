#ifndef __BC_SESSION_MEMORY_H__
#define __BC_SESSION_MEMORY_H__

#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "sds.h"

struct edc_scheduler;
struct edc_session;
struct edc_session_manager;
struct edc_config;
struct edc_task;
struct edc_task_group;

/* L2: session-level memory.

 * Two halves, each with a single writer:
 *   - the EVENT TABLE (a change log of entries, monotonic seq): written by the L2
 *     task. It injects nothing into any task's context -- each task keeps a
 *     watermark (`task->memo_seen_seq`), and request assembly takes the difference
 *     as a "patch" injected at the tail.
 *   - the ENTRY TABLE (index + detail): NOTHING of it is resident in a request -- the head
 *     carries only the constant hint (edc_context.c), the index comes on demand
 *     (`memory_list` / `memory_search`) and the detail via `memory_get`. A deleted entry is
 *     REMOVED for real (see edc_session_memory_delete).

 * Key invariants (same family as L1):
 *   - event seq increases monotonically and is NEVER reused; `next_seq` = the next
 *     seq to assign (0 when no events). A task's "seen" watermark compares against
 *     it: `seq > memo_seen_seq`.
 *   - L2 has no watermark of its own: the trigger is the "fold landed" event and the
 *     input is the just-landed group memory snapshot, with L2 and L1 serialized by a
 *     dependency edge, so the same material never runs twice. */

enum edc_memo_event_kind
{
    EDC_MEMO_CREATED,
    EDC_MEMO_UPDATED,
    EDC_MEMO_SUPERSEDED,
    /* Logically deleted: kept in the table (auditable, retrievable by id) but GONE from
     * every view -- memory_list / memory_search no longer show it. A later compact drops
     * it for real. */
    EDC_MEMO_DELETED,
};

struct edc_memo_event
{
    uint64_t seq;
    /* When this change happened (epoch seconds; 0 = unknown, an old archive). The
     * notice renders it so a reader can tell "how fresh" a change is. seq stays the
     * authoritative order -- this clock can drift. */
    uint64_t ts;
    char *entry_id;
    enum edc_memo_event_kind kind;
    char *title;
    char *summary;
};

struct edc_memo_entry
{
    char *id;
    char *title;
    char *summary;
    sds detail;        /* full detail: not resident in context, fetched by memory_get */
    char *sources;     /* origin: group id / completion_seq range covered (audit) */
    char *proposed_by; /* who proposed it (foreground memory_write's task id; NULL for the curator) */
    uint64_t created_seq;
    uint64_t updated_seq;
    /* Wall-clock (epoch seconds; 0 = unknown, an old archive). `updated_seq` is still
     * the authoritative "rev" for optimistic concurrency and ordering; these are the
     * human-facing timestamps shown in the index / detail / notices so a reader can
     * judge how fresh the entry is. */
    uint64_t created_at;
    uint64_t updated_at;
    bool superseded;
};

struct edc_session_memory
{
    struct edc_memo_event *events;
    size_t event_count, event_cap;

    struct edc_memo_entry *entries;
    size_t entry_count, entry_cap;

    uint64_t next_seq;      /* next seq to assign (0 = no events yet) */
    uint64_t next_entry_id; /* next entry number (id = "m<n>", never reused) */
    uint64_t pruned_before; /* events with seq <= this are gone (advanced by trim_events) */
    int fail_count;         /* consecutive failures, in-memory only */
    /* The entry count the last compaction round was started at. A round that could not get
     * under the cap must not start another one right away: the trigger only fires again once
     * the memory GROWS past this. In-memory only (a restart may retry once). */
    size_t compact_at_entries;
};

extern const char *edc_memo_event_kind_name(enum edc_memo_event_kind kind);
extern enum edc_memo_event_kind edc_memo_event_classify_kind(const char *name);

/* Output cap for a curation call and cap on total entries (an over-cap batch is
 * rejected whole -- better to record nothing than let the index run away). Same
 * order as the fold summary cap: ops carry detail and the corpus can be a large
 * group memory -- 8192 proved too tight (output truncated by length -> whole batch
 * dropped, invisibly). */
#define EDC_SESSION_MEMORY_MAX_TOKENS 32768
#define EDC_SESSION_MEMORY_MAX_ENTRIES 200

/* ── Trimming the event log (the log must not grow forever) ───
 * Pure data, no LLM: the log is a change FEED, not the knowledge, so it is trimmed to the
 * newest KEEP_EVENTS on the write path every time it passes TRIM_EVENTS -- "periodic" in
 * this codebase's sense (a threshold at a natural boundary), not a timer. The ENTRY table
 * is bounded differently: by the compaction task (see EDC_SESSION_MEMORY_COMPACT_ENTRIES). */
#define EDC_SESSION_MEMORY_TRIM_EVENTS 512
#define EDC_SESSION_MEMORY_KEEP_EVENTS 256

/* Compaction trigger: once the entry table holds MORE than this, the host starts a
 * compaction task -- an ordinary system task (sink SESSION_MEMORY, same nature as the fold
 * task) with the memory tools, prompted to merge / condense / drop and rebuild the index,
 * so the entry count and the total volume come back down. A trigger, not a wall: writes
 * are never refused. */
#define EDC_SESSION_MEMORY_COMPACT_ENTRIES EDC_SESSION_MEMORY_MAX_ENTRIES

extern struct edc_session_memory *edc_session_memory_new(void);
extern void edc_session_memory_free(struct edc_session_memory *m);

/* Append one event (returns its seq; 0 when allocation failed). */
extern uint64_t edc_session_memory_add_event(struct edc_session_memory *m, const char *entry_id,
                                             enum edc_memo_event_kind kind, const char *title, const char *summary);

/* Entry: lookup / upsert (by id; update when present). Returns a pointer owned by m. */
extern struct edc_memo_entry *edc_session_memory_entry(struct edc_session_memory *m, const char *id);
extern struct edc_memo_entry *edc_session_memory_upsert_entry(struct edc_session_memory *m, const char *id,
                                                              const char *title, const char *summary,
                                                              const char *detail, const char *sources);

/* Record who proposed the entry (foreground memory_write; owned by m). */
extern void edc_session_memory_note_proposer(struct edc_memo_entry *entry, const char *task_id);

/* ── Readers/writers for tools and notifications ────────────── */

/* Index rows (one per line, "- [m12] title -- summary"; retired ones add
 * "(retired)"), ascending by id for stable paging. The sds is the caller's;
 * `*out_total` gives the entry count (may be NULL). */
extern sds edc_session_memory_index_lines(const struct edc_session_memory *m, size_t offset, size_t n,
                                          size_t *out_total);

/* Search: case-insensitive substring match on title/summary/detail; returns index
 * rows of hits (at most cap), `*out_hits` gives the hit count (may be NULL). */
extern sds edc_session_memory_search_lines(const struct edc_session_memory *m, const char *query, size_t cap,
                                           size_t *out_hits);

/* Regex search: `pattern` is a POSIX ERE (grep -E), case-insensitive, matched anywhere
 * in title/summary/detail. Same rows/ordering/cap semantics as the substring search.
 * `*out_regerror` (may be NULL) = 1 when the pattern is refused (invalid or too long);
 * the result is empty then. */
extern sds edc_session_memory_search_regex_lines(const struct edc_session_memory *m, const char *pattern, size_t cap,
                                                 size_t *out_hits, int *out_regerror);

/* Tail notice (a request-time temporary):
 * entries CREATED/UPDATED since `seen`, one line each (an id appears once, the
 * newest), at most cap, with the total count and a usage hint. NULL = nothing new
 * (do not inject). `*out_max_injected` (may be NULL) = the largest event seq
 * ACTUALLY injected -- the watermark advances to it only once the model really read
 * the request; truncated old events are marked seen by the watermark, and the
 * notice's "N more" plus memory_list/memory_search are their fallback. */
extern sds edc_session_memory_notice(const struct edc_session_memory *m, uint64_t seen, size_t cap,
                                     uint64_t *out_max_injected);

/* Full detail of one entry (falls back to the index row when there is none); NULL
 * when the entry does not exist. */
extern sds edc_session_memory_get_detail(const struct edc_session_memory *m, const char *id);

/* Create an entry: allocate a new id (m<n>, never reused) and write
 * title/summary/detail. Returns the new id (sds, caller sdsfree); NULL on failure.
 * Does NOT write an event -- the caller (foreground memory_write or L2 landing)
 * appends it per ops semantics. */
extern sds edc_session_memory_create_entry(struct edc_session_memory *m, const char *title, const char *summary,
                                           const char *detail, const char *proposed_by);

/* Birth-point adoption: a new task's "seen" watermark = 0 -- the one meaning "no
 * latest entry", so its first request carries a backfill notice (newest cap entries
 * + "N more"). This fixes "a newly created group in a session cannot see the latest
 * session memory". Call ONLY at a task's birth point (user task = create_task, fork
 * child / new group leader = create_group, system task = start_system_task). */
extern void edc_session_memory_adopt_task(struct edc_session *session, struct edc_task *task);

/* ── L2 curation task ──────────────────────────────────────── */

/* The curation call's system text (a self-contained request: this plus its turn). */
extern const char *edc_session_memory_system_prompt(void);

/* The compaction call's system text. A compaction task shares the curation task's sink
 * (SESSION_MEMORY) and tools, so it is told apart by its own prompt: this one asks for
 * MERGING / CONDENSING / DROPPING and an index rebuild, i.e. to make the memory smaller --
 * not to reconcile it with new material. */
extern const char *edc_session_memory_compact_prompt(void);

/* Fold landed -> build the L2 curation task and chain it:
 *     folded group's non-terminal members --SEQUENCE--> L2 --SEQUENCE--> L1 (just landed)
 * so member recovery happens after curation and L2 can read the just-landed group
 * memory. Returns the built L2 task (NULL on failure). */
extern struct edc_task *edc_session_memory_start(struct edc_scheduler *sched, struct edc_session *session,
                                                 struct edc_task_group *folded_group, struct edc_task *fold_task);

/* Result of a guarded (optimistic-concurrency) mutation. */
enum edc_memo_write_result
{
    EDC_MEMO_WRITE_OK = 0,
    EDC_MEMO_WRITE_NOT_FOUND, /* no entry with that id */
    EDC_MEMO_WRITE_CONFLICT,  /* expected_rev != the entry's current updated_seq */
};

/* Update an existing entry in place: given fields overwrite (NULL leaves the old
 * value), and an UPDATED event is appended. Guards against lost updates: the caller
 * passes the `expected_rev` it based the edit on (the entry's `updated_seq`, surfaced by
 * memory_get and by a write's result); a mismatch is refused and nothing changes --
 * never overwrite a newer edit with a stale snapshot. *out_rev (may be NULL) receives the
 * entry's updated_seq (the new one on OK, the current one on CONFLICT).
 */
extern enum edc_memo_write_result edc_session_memory_update(struct edc_session_memory *m, const char *id,
                                                            const char *title, const char *summary, const char *detail,
                                                            uint64_t expected_rev, uint64_t *out_rev);

/* Mark entries retired (tombstone -- content is kept, and it stays visible as "(retired)").
 * For "make this disappear from the views" use edc_session_memory_delete instead. Each
 * NEWLY-retired one gets a SUPERSEDED event. Retire is ID-BASED: it carries no version guard
 * (it does not overwrite content). ATOMIC: if ANY id is unknown, nothing changes and false is
 * returned; on success every id is retired (idempotent -- an already-retired id is a
 * no-op, no duplicate event). */
extern bool edc_session_memory_supersede(struct edc_session_memory *m, const char *const *ids, size_t n);

/* Delete entries FOR REAL: the rows are removed from the table (and from the data -- they
 * are not carried through save/load), so they leave every view and can no longer be read by
 * id. Each one gets a DELETED event first, so a task that already knew it is told it is
 * gone. Same shape as supersede: id-based (no version guard), ATOMIC (any unknown id =
 * nothing changes). NOT idempotent: the row is gone, so deleting it again is an unknown id.
 * For "keep the content, just mark it obsolete" use edc_session_memory_supersede. */
extern bool edc_session_memory_delete(struct edc_session_memory *m, const char *const *ids, size_t n);

/* Trim the event log to the newest EDC_SESSION_MEMORY_KEEP_EVENTS, advancing `pruned_before`
 * to the highest dropped seq. Returns how many events were dropped. Runs automatically on
 * the write path once the log passes EDC_SESSION_MEMORY_TRIM_EVENTS; exposed for tests and
 * for a forced run. (The entry TABLE is bounded by the compaction task, not by this.) */
extern size_t edc_session_memory_trim_events(struct edc_session_memory *m);

/* ── Compaction task: an LLM round that shrinks the memory ──── */

/* True when this session's memory has outgrown its entry cap and no compaction round is in
 * flight, so the host should start one. "In flight" is the session's system task (one at a
 * time, the same gate the fold uses); the memory itself remembers the count it was last
 * compacted at, so a round that could not get under the cap does not immediately start
 * another one (the trigger only fires again once the memory GROWS again). */
extern bool edc_session_memory_compact_due(struct edc_session *session);

/* Attach |task| to a compaction round in flight for its session (the same TASK_DEP_BARRIER
 * the round's start sweeps over the tasks that already exist). Returns how many edges were
 * added (0 = no round in flight). Called at the same two doors as
 * edc_group_fold_hold_task -- creation and resume. */
extern int edc_session_memory_hold_task(struct edc_task *task);

/* Start the compaction round: a system task (sink SESSION_MEMORY, its own turn) holding the
 * whole memory as its material and the agent's compaction instructions. Returns the task
 * (NULL when refused: no session/memo, or a system task is already in flight).
 * The caller persists. */
extern struct edc_task *edc_session_memory_start_compact(struct edc_scheduler *sched, struct edc_session *session);

/* Manual curation (`/session memo compact [focus]`): run once now, no waiting for
 * a fold -- material = every group's memory + the current entry index (+ focus).
 * Refused while a system task is in flight (one at a time, same gate as the
 * automatic path). */
extern struct edc_task *edc_session_memory_start_manual(struct edc_scheduler *sched, struct edc_session *session,
                                                        const char *focus);

/* ── L2 extraction: an idle group's unfolded history ─────────
 *
 * The fold chain only curates what a fold actually covered. A group that never
 * reaches the fold threshold -- or is simply never folded by hand -- keeps its
 * committed history forever outside L2. This third write path closes that gap:
 * once a user group has been idle long enough and still holds committed messages
 * past its `turn->memo_prefix` watermark, run ONE self-contained curation call
 * over that raw history. It is dispatched as an ordinary system task
 * (sink SESSION_MEMORY, memo_kind EXTRACT) and its only post-hoc action is
 * advancing the watermark. */
#define EDC_MEMO_EXTRACT_IDLE_SECS 600
#define EDC_MEMO_EXTRACT_MAX_FAILURES 3

/* The extraction call's system text (self-contained: this plus its turn). Distinct
 * from the curator's, which says the material "was just folded" -- it was not. */
extern const char *edc_session_memory_extract_prompt(void);

/* How many committed messages of |group| are still past their turn's memo watermark
 * (the material an EXTRACT round would carry). Members with a memory sink are skipped.
 */
extern size_t edc_task_group_extractable_msg_count(struct edc_task_group *group);

/* Is |group| due for extraction at wall-clock |now|? Conditions: a USER group, at least
 * one member, every member finished (idle), something extractable, a fold not due (fold
 * has priority), no fold/curation in flight for it, and its failure streak under the cap.
 * The idle clock is the newest committed message's timestamp. */
extern bool edc_session_memory_extract_due(struct edc_task_group *group, const struct edc_config *conf, time_t now);

/* Scan every session and start at most one extraction. When one started, `*out_session`
 * (optional) is the owning session so the caller persists the new task. */
extern bool edc_session_memory_extract_try(struct edc_scheduler *sched, struct edc_session_manager *sessions,
                                           struct edc_config *conf, struct edc_session **out_session);

/* Landing: advance the memo watermark by the cut the task carries (members before the cut
 * whole, the cut's turn up to its prefix). Returns true when it advanced; false when the
 * cut does not resolve (then nothing changes and one failure is counted). */
extern bool edc_session_memory_extract_land(struct edc_task *task);

/* Consecutive-failure accounting for extraction (automatic path only): at the cap the
 * automatic rounds stop until the group gets new committed content or a manual fold.
 * Reset on a successful landing and on new committed content. In-memory only. */
extern void edc_session_memory_extract_note_failure(struct edc_task_group *group);
extern void edc_session_memory_extract_note_progress(struct edc_task_group *group);

/* Text for `/session memo show` (pure function, easy to assert): empty id = list
 * all entries + total; an id = that entry's full content (or a hint when missing).
 * Caller sdsfree. */
extern sds edc_session_memory_show_text(const struct edc_session_memory *m, const char *id);

/* Text for `/session memo search <q>` (pure function): index rows of hits + count. */
extern sds edc_session_memory_search_text(const struct edc_session_memory *m, const char *query);

/* L2 landing is retired: the curation task writes
 * through the memory tools during its run, so there is no post-hoc ops landing. */

/* Persistence (schema v7's `session_memory` block). */
extern cJSON *edc_session_memory_to_json(const struct edc_session_memory *m);
extern struct edc_session_memory *edc_session_memory_from_json(const cJSON *json);

#endif
