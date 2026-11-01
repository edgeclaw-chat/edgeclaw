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

/* Which fields a change actually touched (bit mask on edc_memo_event.changed). The
 * notice renders an `updated` line from this instead of re-printing the summary, so a
 * reader can tell "a decision-bearing field changed, worth a look" from "the wording was
 * tightened". 0 = an old archive (unknown): fall back to the plain rendering. */
#define EDC_MEMO_F_TITLE (1u << 0)
#define EDC_MEMO_F_SUMMARY (1u << 1)
#define EDC_MEMO_F_DETAIL (1u << 2)
#define EDC_MEMO_F_RETIRED (1u << 3)

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
    /* EDC_MEMO_F_* bits. Computed inside the mutator (it still holds the old value), so
     * no caller has to pass it -- and no call site can forget to. */
    uint32_t changed;
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
    /* The entry count the last compaction task was started at. A compaction that could not
     * get under the cap must not immediately start another one: the trigger only fires again
     * once the memory GROWS past this. In-memory only (a restart may retry once). */
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

/* ── Compaction's material: the whole index, never a slice ────
 * The task's material is the INDEX only (id / rev / updated / title / summary, never the
 * detail): the model calls `memory_get` for the entries it actually intends to rewrite, so
 * the request stays small however long the entries are.
 *
 * The index is given WHOLE, every time: it is cheap by construction (one line per entry, the
 * summary clipped at COMPACT_SUMMARY_MAX), and only the whole picture shows that m7 and m148
 * are the same subject and belong together. Slicing it would hide exactly the cross-entry
 * relations compaction exists to find -- shrinking the material was never the point, the
 * index+summary IS the small thing.
 *
 * Because the material is small, one ordinary task suffices: the normal tool loop lets it
 * read and write as many entries as it needs, in as many turns as it needs, and each
 * response stays short (a few tool calls, one line of text). What used to break compaction
 * was asking ONE RESPONSE to rewrite the whole table: that hit max_tokens, and a truncated
 * response carries no tool calls, so nothing landed at all (see edc_executor.c). Nothing
 * here needs its own round/iteration budget -- config `max_iter` governs, as for any task.
 */

extern struct edc_session_memory *edc_session_memory_new(void);
extern void edc_session_memory_free(struct edc_session_memory *m);

/* Append one event (returns its seq; 0 when allocation failed). */
/* Restore one row verbatim (the load path). These take the stored seq / timestamps because
 * the mutators above assign them; restoring through those would renumber the log. */
extern struct edc_memo_event *edc_session_memory_restore_event(struct edc_session_memory *m, uint64_t seq, uint64_t ts,
                                                              const char *entry_id, const char *kind_name,
                                                              const char *title, const char *summary,
                                                              uint32_t changed);
extern struct edc_memo_entry *edc_session_memory_restore_entry(struct edc_session_memory *m, const char *id,
                                                              const char *title, const char *summary,
                                                              const char *detail, const char *sources,
                                                              const char *proposed_by, uint64_t created_seq,
                                                              uint64_t updated_seq, uint64_t created_at,
                                                              uint64_t updated_at, bool superseded);

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

/* ── The catalogue channel: "what exists" ─────────────────────
 * The counterpart of the notice. The notice answers "what changed" (precise, priced,
 * per-change, FIFO); the catalogue answers "what is available" (cheap rows, once per
 * task, id-ordered so `memory_list` can continue it).
 *
 * Why it exists at all: without it a new task cannot see WHICH memories exist, and an
 * agent with no lead almost never calls memory_search -- so the whole L2 layer sits
 * unused. Giving the catalogue nothing (the original plan, `baseline_sample = 0`) saves
 * ~1.2k tokens and costs the feature.
 *
 * Why it can be cheap: id + title only, ~33 tok per row measured (vs ~122 for a notice
 * row). 2000 tokens covers ~60 entries, i.e. the great majority of real sessions in full
 * (measured sessions: 8, 37, 84 live entries).
 *
 * Rendered from the ENTRY table, never the event table: DELETED events keep tombstones,
 * and listing from them once produced "共 37 条" above "… 70 more".
 *
 * NULL when the memory is empty (inject nothing). */
#define EDC_SESSION_MEMORY_CATALOGUE_TOKENS 2000
extern sds edc_session_memory_catalogue(const struct edc_session_memory *m, size_t token_budget);

/* Tail notice (a request-time temporary):
 * entries CREATED/UPDATED since `seen`, one line each (an id appears once, the newest),
 * at most `cap`, with the changed count AND the total count in the header.
 * NULL = nothing new (do not inject).
 *
 * DELIVERY SEMANTICS (2026-10-30): the window is FIFO -- the OLDEST pending change goes
 * first -- and `*out_max_injected` (may be NULL) is the largest event seq ACTUALLY
 * delivered, so the watermark is a cursor and the next request continues where this one
 * stopped. Every change is therefore either delivered or explicitly declined: past
 * `cap * 3` pending changes the notice stops listing and says so ("backlog too large"),
 * advancing the watermark past them. The earlier pair (newest `cap` + advance to
 * `next_seq`) was self-consistent as a summary, but silently marked unseen changes seen. */
extern sds edc_session_memory_notice(const struct edc_session_memory *m, uint64_t seen, size_t cap,
                                     uint64_t *out_max_injected);

/* Full detail of one entry (falls back to the index row when there is none); NULL
 * when the entry does not exist. */
extern sds edc_session_memory_get_detail(const struct edc_session_memory *m, const char *id);

/* ── summary ⊆ detail: the fact-coverage invariant ─────────────
 * The three fields are a hierarchy (measured on a real table: title 29 tok, summary 100,
 * detail 1079 -- 1 : 3.4 : 38): title is the ADDRESS, summary is the DECISION PAYLOAD (it
 * is what a reader sees when many entries sit side by side and it must not fetch each
 * detail), detail is the STORE. That only works if a reader who stops at the summary does
 * not end up holding a fact it can never resolve -- so every HARD fact named in the summary
 * must also appear in the detail.
 *
 * HARD = commit ids and snake_case identifiers. Both are unrewritable: if the summary says
 * `4dd2b14` or `edc_sub_fn`, no paraphrase carries it and no reader can re-derive it.
 * Measured on the real 129-entry table: 237 hard facts, 3 uncovered (1.3%) -- all three
 * genuine defects (a lost commit id and two symbol names).
 *
 * NOT enforced, deliberately: paths and quantities with units. `200us` vs `0.2ms` and
 * `/log/x-YYYY-MM-DD.log` vs "the daily log" are legitimate rewrites, and hard-failing
 * them blocks honest writes -- 9.7% of them look uncovered while most are formatting.
 *
 * A literal `summary inside detail` check is NOT the invariant and cannot be used: the
 * summary is always a rewrite (0 of 129 matched as a substring). It is about FACTS, not
 * text. Returns the number of uncovered hard facts; `*out_report` (may be NULL) receives a
 * comma-separated list of them, for the error message. No detail => 0: with no second home
 * for its facts a summary-only entry has no coverage to demand. */
extern size_t edc_session_memory_uncovered_facts(const char *summary, const char *detail, sds *out_report);

/* Create an entry: allocate a new id (m<n>, never reused) and write
 * title/summary/detail. Returns the new id (sds, caller sdsfree); NULL on failure.
 * Does NOT write an event -- the caller (foreground memory_write or L2 landing)
 * appends it per ops semantics. */
extern sds edc_session_memory_create_entry(struct edc_session_memory *m, const char *title, const char *summary,
                                           const char *detail, const char *proposed_by);

/* Birth-point adoption: a new task has received nothing -- neither a change notice nor
 * the CATALOGUE -- so both pieces of state are cleared (`memo_seen_seq = 0`,
 * `memo_baseline_done = false`). Its first request therefore carries the index card
 * ("what exists", id + title, budget-bounded) instead of a change notice.
 *
 * Why the catalogue instead of a backfill delta: a delta's meaning is "these are the
 * changes since you last looked", which says nothing to a task that never looked. Reusing
 * the change channel to ship an index also made a fresh task look like it had just
 * witnessed a burst of changes. The catalogue states the actual fact and costs ~4x less
 * per row (id + title vs kind + title + summary + timestamp).
 *
 * Call ONLY at a task's birth point (user task = create_task, fork child / new group
 * leader = create_group, system task = start_system_task). */
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

/* ── Compaction task: one LLM task that shrinks the memory ──── */

/* True when this session's memory has outgrown its entry cap and no system task is in
 * flight, so the host should start a compaction. Read-only. "In flight" is the session's
 * system task (one at a time, the same gate the fold uses); the memory remembers the count
 * it was last started at, so a compaction that could not get under the cap does not
 * immediately start another one (the trigger only fires again once the memory GROWS). */
extern bool edc_session_memory_compact_due(struct edc_session *session);

/* Attach |task| to a compaction in flight for its session (the same TASK_DEP_BARRIER the
 * start sweeps over the tasks that already exist). Returns how many edges were added
 * (0 = no compaction in flight). Called at the same two doors as
 * edc_group_fold_hold_task -- creation and resume. */
extern int edc_session_memory_hold_task(struct edc_task *task);

/* Start a compaction: a system task (sink SESSION_MEMORY, its own turn) whose
 * material is the WHOLE entry index (no detail) and the compaction instructions.
 * Returns the task (NULL when refused: no session/memo, or a system task is already in
 * flight). The caller persists. */
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
 * one member, every member finished and it has been so for EDC_MEMO_EXTRACT_IDLE_SECS,
 * something extractable, a fold not due (fold has priority), no fold/curation in flight
 * for it, and its failure streak under the cap. The idle clock starts when the group
 * BECOMES idle (group->idle_since, maintained by edc_task_group_refresh_busy) -- NOT at
 * the newest message's timestamp: a long-running task that was just stopped makes the
 * group idle only now. */
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
