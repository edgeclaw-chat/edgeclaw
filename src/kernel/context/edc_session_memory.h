#ifndef __BC_SESSION_MEMORY_H__
#define __BC_SESSION_MEMORY_H__

#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sds.h"

struct edc_scheduler;
struct edc_session;
struct edc_task;
struct edc_task_group;

/* L2: session-level memory.

 * Two halves, each with a single writer:
 *   - the EVENT TABLE (a change log of entries, monotonic seq): written by the L2
 *     task. It injects nothing into any task's context -- each task keeps a
 *     watermark (`task->memo_seen_seq`), and request assembly takes the difference
 *     as a "patch" injected at the tail.
 *   - the ENTRY TABLE (index + detail): the index is resident in the head each
 *     round, the detail is on demand (`memory_get`).

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
};

struct edc_memo_event
{
    uint64_t seq;
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
    uint64_t pruned_before; /* lower bound after event pruning (P4e; always 0 now) */
    int fail_count;         /* consecutive failures, in-memory only */
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

/* Mark entries retired (tombstone -- content is kept, never deleted); each NEWLY-retired
 * one gets a SUPERSEDED event. Retire is ID-BASED: it carries no version guard (it does
 * not overwrite content). ATOMIC: if ANY id is unknown, nothing changes and false is
 * returned; on success every id is retired (idempotent -- an already-retired id is a
 * no-op, no duplicate event). */
extern bool edc_session_memory_supersede(struct edc_session_memory *m, const char *const *ids, size_t n);

/* Manual curation (`/session memo compact [focus]`): run once now, no waiting for
 * a fold -- material = every group's memory + the current entry index (+ focus).
 * Refused while a system task is in flight (one at a time, same gate as the
 * automatic path). */
extern struct edc_task *edc_session_memory_start_manual(struct edc_scheduler *sched, struct edc_session *session,
                                                        const char *focus);

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
