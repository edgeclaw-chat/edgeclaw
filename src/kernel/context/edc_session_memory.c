#include "edc_session_memory.h"

#include "edc_config.h"
#include "edc_datetime_utils.h"
#include "edc_group_fold.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_token_estimate.h"
#include "edc_turn.h"

#include "edc_compat.h"
#include "log.h"
#include "utlist_fix.h"

#include <ctype.h>
#ifdef _WIN32
/* Windows ships no POSIX <regex.h>; PCRE2 provides a drop-in wrapper. */
#include <pcre2posix.h>
#else
#include <regex.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Vocabulary ────────────────────────────────────────────── */

const char *edc_memo_event_kind_name(enum edc_memo_event_kind kind)
{
    switch (kind)
    {
    case EDC_MEMO_CREATED:
        return "created";
    case EDC_MEMO_UPDATED:
        return "updated";
    case EDC_MEMO_SUPERSEDED:
        return "superseded";
    case EDC_MEMO_DELETED:
        return "deleted";
    }
    return "created";
}

enum edc_memo_event_kind edc_memo_event_classify_kind(const char *name)
{
    if (!name)
        return EDC_MEMO_CREATED;
    if (strcmp(name, "updated") == 0)
        return EDC_MEMO_UPDATED;
    if (strcmp(name, "superseded") == 0)
        return EDC_MEMO_SUPERSEDED;
    if (strcmp(name, "deleted") == 0)
        return EDC_MEMO_DELETED;
    return EDC_MEMO_CREATED;
}

/* ── Lifetime ──────────────────────────────────────────────── */

struct edc_session_memory *edc_session_memory_new(void)
{
    return g_new0(struct edc_session_memory, 1);
}

static void memo_event_free(struct edc_memo_event *e)
{
    free(e->entry_id);
    free(e->title);
    free(e->summary);
}

static void memo_entry_free(struct edc_memo_entry *e)
{
    free(e->id);
    free(e->title);
    free(e->summary);
    free(e->sources);
    free(e->proposed_by);
    if (e->detail)
        sdsfree(e->detail);
}

void edc_session_memory_free(struct edc_session_memory *m)
{
    if (!m)
        return;
    for (size_t i = 0; i < m->event_count; i++)
        memo_event_free(&m->events[i]);
    free(m->events);
    for (size_t i = 0; i < m->entry_count; i++)
        memo_entry_free(&m->entries[i]);
    free(m->entries);
    free(m);
}

/* ── Helpers ───────────────────────────────────────────────── */

static void *grow_array(void *data, size_t *cap, size_t elem_size, size_t need)
{
    if (need <= *cap)
        return data;
    size_t next = (*cap == 0) ? 8 : (*cap * 2);
    while (next < need)
        next *= 2;
    void *grown = realloc(data, next * elem_size);
    if (!grown)
        return NULL;
    *cap = next;
    return grown;
}

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* Trims the event log when it has outgrown its threshold (defined below, next to the trim
 * itself). Forward-declared because every mutator calls it. */
static void memo_trim_if_due(struct edc_session_memory *m);

/* ── Event table ───────────────────────────────────────────── */

/* Append an event with a PREDETERMINED seq (an assembly internal: the writer supplies seq, load uses the on-disk seq). */
static struct edc_memo_event *append_event(struct edc_session_memory *m, uint64_t seq, uint64_t ts, const char *entry_id,
                                           enum edc_memo_event_kind kind, const char *title, const char *summary)
{
    if (!m || !entry_id || seq == 0)
        return NULL;
    void *grown = grow_array(m->events, &m->event_cap, sizeof(*m->events), m->event_count + 1);
    if (!grown)
        return NULL;
    m->events = grown;

    struct edc_memo_event *e = &m->events[m->event_count];
    memset(e, 0, sizeof(*e));
    e->seq = seq;
    e->ts = ts;
    e->entry_id = dup_or_null(entry_id);
    e->kind = kind;
    e->title = dup_or_null(title);
    e->summary = dup_or_null(summary);
    m->event_count++;
    return e;
}

/* ── Restore (load path) ───────────────────────────────────────
 *
 * The store reads rows and must reproduce what was saved EXACTLY -- the seq that orders the
 * notice, the timestamps the index shows, the entry's created_seq/updated_seq which are the
 * optimistic-concurrency "rev". The mutators above assign those themselves (add_event stamps
 * "now", upsert_entry leaves them to the caller), so restoring through them would renumber
 * the log on every restart. These take the values as stored.
 *
 * They live here, not in the store, because this module owns the shape: whoever else needs
 * to read or write these tables calls these rather than touching the arrays. */
struct edc_memo_event *edc_session_memory_restore_event(struct edc_session_memory *m, uint64_t seq, uint64_t ts,
                                                       const char *entry_id, const char *kind_name,
                                                       const char *title, const char *summary)
{
    return append_event(m, seq, ts, entry_id, edc_memo_event_classify_kind(kind_name), title, summary);
}

struct edc_memo_entry *edc_session_memory_restore_entry(struct edc_session_memory *m, const char *id,
                                                       const char *title, const char *summary, const char *detail,
                                                       const char *sources, const char *proposed_by,
                                                       uint64_t created_seq, uint64_t updated_seq,
                                                       uint64_t created_at, uint64_t updated_at, bool superseded)
{
    if (!m || !id)
        return NULL;
    void *grown = grow_array(m->entries, &m->entry_cap, sizeof(*m->entries), m->entry_count + 1);
    if (!grown)
        return NULL;
    m->entries = grown;

    struct edc_memo_entry *e = &m->entries[m->entry_count++];
    memset(e, 0, sizeof(*e));
    e->id = dup_or_null(id);
    e->title = dup_or_null(title);
    e->summary = dup_or_null(summary);
    e->detail = detail ? sdsnew(detail) : NULL;
    e->sources = dup_or_null(sources);
    e->proposed_by = dup_or_null(proposed_by);
    e->created_seq = created_seq;
    e->updated_seq = updated_seq;
    e->created_at = created_at;
    e->updated_at = updated_at;
    e->superseded = superseded;
    return e;
}

uint64_t edc_session_memory_add_event(struct edc_session_memory *m, const char *entry_id,
                                      enum edc_memo_event_kind kind, const char *title, const char *summary)
{
    if (!m)
        return 0;
    uint64_t seq = m->next_seq + 1;
    if (!append_event(m, seq, (uint64_t)edc_datetime_now(), entry_id, kind, title, summary))
        return 0;
    m->next_seq = seq;
    return seq;
}

/* ── Entry table ───────────────────────────────────────────── */

struct edc_memo_entry *edc_session_memory_entry(struct edc_session_memory *m, const char *id)
{
    if (!m || !id)
        return NULL;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        if (m->entries[i].id && strcmp(m->entries[i].id, id) == 0)
            return &m->entries[i];
    }
    return NULL;
}

struct edc_memo_entry *edc_session_memory_upsert_entry(struct edc_session_memory *m, const char *id,
                                                       const char *title, const char *summary, const char *detail,
                                                       const char *sources)
{
    if (!m || !id)
        return NULL;

    struct edc_memo_entry *e = edc_session_memory_entry(m, id);
    if (!e)
    {
        void *grown = grow_array(m->entries, &m->entry_cap, sizeof(*m->entries), m->entry_count + 1);
        if (!grown)
            return NULL;
        m->entries = grown;
        e = &m->entries[m->entry_count];
        memset(e, 0, sizeof(*e));
        m->entry_count++;
        e->id = dup_or_null(id);
    }

    free(e->title);
    free(e->summary);
    free(e->sources);
    if (e->detail)
        sdsfree(e->detail);
    e->title = dup_or_null(title);
    e->summary = dup_or_null(summary);
    e->sources = dup_or_null(sources);
    e->detail = detail ? sdsnew(detail) : NULL;
    return e;
}

/* ── Readers/writers for tools and notifications ───────────── */

/* Entries in ascending id order (the numeric part of "m12") -- keeps memory_list paging stable. */
static int entry_id_num(const char *id)
{
    if (!id || id[0] != 'm')
        return 0;
    return atoi(id + 1);
}

static sds index_line_for(const struct edc_memo_entry *e)
{
    sds line = sdscatprintf(sdsempty(), "- [%s]%s %s", e->id ? e->id : "?",
                            (e->superseded ? " (retired)" : ""), e->title ? e->title : "(untitled)");
    if (e->summary && e->summary[0])
        line = sdscatprintf(line, " — %s", e->summary);
    /* When it was last touched (falls back to created for an untouched entry), so a
     * reader can tell how fresh the entry is. Appended at the END so line-shaped
     * consumers (and tests) that match "[id] title" are unaffected. */
    sds when = edc_datetime_format_min((time_t)(e->updated_at ? e->updated_at : e->created_at));
    line = sdscatprintf(line, " (%s)", when);
    sdsfree(when);
    return line;
}

sds edc_session_memory_index_lines(const struct edc_session_memory *m, size_t offset, size_t n, size_t *out_total)
{
    if (out_total)
        *out_total = m ? m->entry_count : 0;
    if (!m || n == 0 || m->entry_count == 0)
        return sdsempty();

    /* Collect and sort ascending by id number (few entries; insertion sort). */
    struct edc_memo_entry **sorted = malloc(m->entry_count * sizeof(*sorted));
    if (!sorted)
        return sdsempty();
    size_t k = 0;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        size_t j = k++;
        while (j > 0 && entry_id_num(sorted[j - 1]->id) > entry_id_num(m->entries[i].id))
        {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = &m->entries[i];
    }

    sds out = sdsempty();
    for (size_t i = offset; i < k && i < offset + n; i++)
    {
        if (sdslen(out))
            out = sdscat(out, "\n");
        sds line = index_line_for(sorted[i]);
        out = sdscatsds(out, line);
        sdsfree(line);
    }
    free(sorted);
    return out;
}

static bool contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0])
        return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
    {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

sds edc_session_memory_search_lines(const struct edc_session_memory *m, const char *query, size_t cap, size_t *out_hits)
{
    if (out_hits)
        *out_hits = 0;
    if (!m || !query || !query[0] || cap == 0 || m->entry_count == 0)
        return sdsempty();

    /* Hit indices, output in descending updated_seq (falling back to created_seq).
     * Search is a glance; stable ordering is not required. */
    size_t *hits = malloc(m->entry_count * sizeof(*hits));
    if (!hits)
        return sdsempty();
    size_t nhits = 0;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        if (contains_ci(e->title, query) || contains_ci(e->summary, query) ||
            (e->detail && contains_ci(e->detail, query)))
        {
            size_t j = nhits++;
            while (j > 0)
            {
                const struct edc_memo_entry *prev = &m->entries[hits[j - 1]];
                uint64_t a = prev->updated_seq ? prev->updated_seq : prev->created_seq;
                uint64_t b = e->updated_seq ? e->updated_seq : e->created_seq;
                if (a >= b)
                    break;
                hits[j] = hits[j - 1];
                j--;
            }
            hits[j] = i;
        }
    }

    sds out = sdsempty();
    for (size_t i = 0; i < nhits && i < cap; i++)
    {
        if (sdslen(out))
            out = sdscat(out, "\n");
        sds line = index_line_for(&m->entries[hits[i]]);
        out = sdscatsds(out, line);
        sdsfree(line);
    }
    free(hits);
    if (out_hits)
        *out_hits = nhits;
    return out;
}

/* Regex search: POSIX ERE (grep -E), case-insensitive, matched ANYWHERE in title /
 * summary / detail. `*out_regerror` (may be NULL) is set to 1 when the pattern is
 * refused (failed to compile, or longer than the bound) -- the returned sds is empty
 * then. Same ordering (newest first) and cap semantics as the substring search. */
#define EDC_MEMO_MAX_PATTERN_LEN 512

sds edc_session_memory_search_regex_lines(const struct edc_session_memory *m, const char *pattern, size_t cap,
                                          size_t *out_hits, int *out_regerror)
{
    if (out_hits)
        *out_hits = 0;
    if (out_regerror)
        *out_regerror = 0;
    if (!m || !pattern || !pattern[0] || cap == 0 || m->entry_count == 0)
        return sdsempty();
    /* Bound the pattern: entries are few and small, but a pathological regex should
     * not be able to run wild. */
    if (strlen(pattern) > EDC_MEMO_MAX_PATTERN_LEN)
    {
        if (out_regerror)
            *out_regerror = 1;
        return sdsempty();
    }

    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0)
    {
        if (out_regerror)
            *out_regerror = 1;
        return sdsempty();
    }

    size_t *hits = malloc(m->entry_count * sizeof(*hits));
    if (!hits)
    {
        regfree(&re);
        return sdsempty();
    }
    size_t nhits = 0;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        bool hit = (e->title && regexec(&re, e->title, 0, NULL, 0) == 0) ||
                   (e->summary && regexec(&re, e->summary, 0, NULL, 0) == 0) ||
                   (e->detail && regexec(&re, e->detail, 0, NULL, 0) == 0);
        if (!hit)
            continue;
        size_t j = nhits++;
        while (j > 0)
        {
            const struct edc_memo_entry *prev = &m->entries[hits[j - 1]];
            uint64_t a = prev->updated_seq ? prev->updated_seq : prev->created_seq;
            uint64_t b = e->updated_seq ? e->updated_seq : e->created_seq;
            if (a >= b)
                break;
            hits[j] = hits[j - 1];
            j--;
        }
        hits[j] = i;
    }
    regfree(&re);

    sds out = sdsempty();
    for (size_t i = 0; i < nhits && i < cap; i++)
    {
        if (sdslen(out))
            out = sdscat(out, "\n");
        sds line = index_line_for(&m->entries[hits[i]]);
        out = sdscatsds(out, line);
        sdsfree(line);
    }
    free(hits);
    if (out_hits)
        *out_hits = nhits;
    return out;
}

sds edc_session_memory_get_detail(const struct edc_session_memory *m, const char *id)
{
    if (!m || !id)
        return NULL;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        if (e->id && strcmp(e->id, id) == 0)
        {
            sds body = (e->detail && sdslen(e->detail) > 0) ? sdsdup(e->detail) : index_line_for(e);
            /* Both timestamps up front: the reader judges freshness before the detail. */
            sds created = edc_datetime_format((time_t)e->created_at);
            sds updated = edc_datetime_format((time_t)e->updated_at);
            sds out = sdscatprintf(sdsempty(), "created: %s\nupdated: %s\n%s", created, updated, body);
            sdsfree(body);
            sdsfree(created);
            sdsfree(updated);
            return out;
        }
    }
    return NULL;
}

sds edc_session_memory_create_entry(struct edc_session_memory *m, const char *title, const char *summary,
                                    const char *detail, const char *proposed_by)
{
    if (!m || !title)
        return NULL;
    sds id = sdscatprintf(sdsempty(), "m%llu", (unsigned long long)(m->next_entry_id + 1));
    struct edc_memo_entry *e = edc_session_memory_upsert_entry(m, id, title, summary, detail, NULL);
    if (!e)
    {
        sdsfree(id);
        return NULL;
    }
    m->next_entry_id++;
    if (proposed_by)
        edc_session_memory_note_proposer(e, proposed_by);
    /* created_seq/updated_seq are filled by the caller from the event seq it appended (memory_write / L2 landing). */
    e->created_at = e->updated_at = (uint64_t)edc_datetime_now();
    memo_trim_if_due(m);
    return id;
}

/* ── L2 curation task ──────────────────────────────────────── */

/* Curation instruction: same family as the fold instruction -- the system section of
 * a self-contained request, constant text (so the L2 call's prefix is stable). The
 * output contract is the six-section summary. */
/* ── Compaction task: an LLM round whose only job is to make the memory smaller ──
 *
 * Same nature as the fold task: a system task (sink SESSION_MEMORY, its own turn, tools =
 * the memory tools) started when the entry table outgrows its cap, prompted to merge /
 * condense / drop and rebuild the index. It has to be told apart from the CURATION task,
 * which shares its sink and tools, so it carries its own system prompt (the request picks
 * it by task->memo_kind, see edc_llm_request.c). */

static const char kCompactSystemPrompt[] =
    "You are compacting this session's shared memory. Nobody is waiting on a conversation: "
    "your only job is to make the memory SMALLER while keeping what is durable.\n"
    "\n"
    "The whole memory is in this turn. You may:\n"
    "- MERGE entries that belong together into one: memory_update the survivor with the union, "
    "then memory_delete the entries it absorbed.\n"
    "- CONDENSE a verbose entry into its decision plus the reason, dropping narration, "
    "superseded plans and anything another entry already covers (memory_update).\n"
    "- DROP entries that are obsolete, wrong, or no longer worth a slot (memory_delete).\n"
    "- REBUILD the index if that serves the goal: decide which entries should exist, write "
    "them, delete the rest. Ids are never reused, but keeping an id is not required.\n"
    "\n"
    "Hard rules:\n"
    "- Never lose durable knowledge to save space. A decision, a constraint, a gotcha or an "
    "external fact the next session would need must survive somewhere.\n"
    "- Keep what cannot be recovered otherwise: paths, commands, error text, numbers.\n"
    "- When two entries cover the same thing, keep the content of the one updated LATER "
    "(the dump shows each entry's `updated`); if you cannot tell, keep both and note it.\n"
    "- Nothing about this round belongs in the memory: do not write an entry saying you "
    "compacted it.\n"
    "- Do not invent facts. If two entries disagree and you cannot tell which is right, keep "
    "both and say so in the summary.\n"
    "\n"
    "Goal: the entry count back under the cap, and the memory materially smaller.\n";

const char *edc_session_memory_compact_prompt(void)
{
    return kCompactSystemPrompt;
}

/* The material: every entry, with the fields a rewrite needs (id, rev, title, summary,
 * detail). The curator's prompt embeds group memories; this one embeds the memory itself,
 * because compacting needs nothing from the outside world. */
static sds build_compact_prompt(struct edc_session *session)
{
    struct edc_session_memory *m = session ? session->memo : NULL;
    sds out = sdscatprintf(sdsempty(),
                           "## Compact the session memory\n"
                           "It holds %zu entries; the cap is %d. Bring it back under the cap and make it\n"
                           "smaller. Every entry follows (id / rev / title / summary / detail); use\n"
                           "memory_update (with the rev as expected_rev) and memory_delete.\n",
                           m ? m->entry_count : (size_t)0, EDC_SESSION_MEMORY_COMPACT_ENTRIES);
    if (!m || m->entry_count == 0)
        return out;

    /* Ids are handed out in ascending order and a delete keeps the array in order, so the
     * dump reads in the same order as memory_list. */
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        sds when = edc_datetime_format_min((time_t)(e->updated_at ? e->updated_at : e->created_at));
        out = sdscatprintf(out, "\n- id: %s  rev: %llu  updated: %s%s\n", e->id, (unsigned long long)e->updated_seq,
                           when, e->superseded ? "  (retired)" : "");
        sdsfree(when);
        out = sdscatprintf(out, "  title: %s\n", e->title ? e->title : "(untitled)");
        if (e->summary && e->summary[0])
            out = sdscatprintf(out, "  summary: %s\n", e->summary);
        if (e->detail && sdslen(e->detail) > 0)
            out = sdscatprintf(out, "  detail: %s\n", e->detail);
    }
    return out;
}

int edc_session_memory_hold_task(struct edc_task *task)
{
    if (!task || !task->group || !task->group->session)
        return 0;
    if (edc_task_is_memory_sink(task))
        return 0; /* the round itself is never held (a finished task is NOT skipped -- see
                   * edc_group_fold_hold_task: the resume door hands us those) */

    struct edc_task_group *sys = edc_session_find_system_group(task->group->session);
    if (!sys)
        return 0;

    int added = 0;
    struct edc_task *t;
    LL_FOREACH2(sys->members, t, group_next)
    {
        if (t == task || edc_task_is_finished(t) || t->memo_kind != EDC_MEMO_KIND_COMPACT)
            continue;
        if (edc_task_add_dependency(task, t, TASK_DEP_BARRIER))
            added++;
    }
    return added;
}

bool edc_session_memory_compact_due(struct edc_session *session)
{
    if (!session || !session->memo)
        return false;
    const struct edc_session_memory *m = session->memo;
    /* Over the cap, and it has GROWN since the last round was started: a round that could
     * not get under the cap must not immediately start another one. */
    if (m->entry_count <= EDC_SESSION_MEMORY_COMPACT_ENTRIES || m->entry_count <= m->compact_at_entries)
        return false;
    /* One system task per session at a time (the same gate the fold uses). */
    return !edc_scheduler_has_pending_system_task(session);
}

struct edc_task *edc_session_memory_start_compact(struct edc_scheduler *sched, struct edc_session *session)
{
    if (!sched || !session || !session->memo)
        return NULL;
    if (edc_scheduler_has_pending_system_task(session))
        return NULL; /* one at a time */

    sds prompt = build_compact_prompt(session);
    struct edc_task *task =
        edc_scheduler_spawn_system_task_as(sched, session, "compact session memory", EDC_SINK_SESSION_MEMORY);
    if (!task)
    {
        sdsfree(prompt);
        return NULL;
    }
    /* Same sink as curation, different prompt: this is what tells the two apart. */
    task->memo_kind = EDC_MEMO_KIND_COMPACT;

    /* Barrier over the WHOLE session: every task that is still going waits for this round, so
     * the memory is not rewritten underneath a task that is mid-flight, and they pick up
     * afterwards. Same shape as the fold's barrier -- and the same safety property: a
     * TASK_DEP_BARRIER is released by its source ending EITHER way, so a compaction round that
     * dies (or is cancelled) releases the tasks it was holding instead of taking them with it.
     *
     * What "waits" means concretely: a task that is between rounds (about to send its next
     * request) parks at AWAIT_DEPS and is woken by the release; one whose LLM call is already
     * in flight finishes that call -- the fold behaves the same way, and it is the request
     * boundary that is the safe point, not the middle of a round. */
    {
        struct edc_task_group *g = NULL;
        DL_FOREACH(session->groups, g)
        {
            struct edc_task *m = NULL;
            LL_FOREACH2(g->members, m, group_next)
            {
                if (m == task || edc_task_is_completed(m) || edc_task_is_cancelled(m) || edc_task_has_failed(m))
                    continue;
                edc_task_add_dependency(m, task, TASK_DEP_BARRIER);
            }
        }
    }
    if (!edc_scheduler_attach_input(sched, task, prompt))
    {
        log_error("L2 memo: could not attach the compaction prompt — nothing started");
        edc_task_mark_failed(task);
        sdsfree(prompt);
        return NULL;
    }
    sdsfree(prompt);
    /* Remember the size we tried at, so a round that cannot shrink below the cap does not
     * start another one until the memory grows again. */
    session->memo->compact_at_entries = session->memo->entry_count;
    log_info("L2 memo compact start: session=%s task=%s entries=%zu", edc_id_str(session->session_id), edc_id_str(task->task_id),
             session->memo->entry_count);
    return task;
}

static const char kCurateSystemPrompt[] =
    "You are the session-memory curator for the EdgeClaw agent.\n"
    "\n"
    "You are given the memory of ONE group that was just folded (appended at the end of\n"
    "your input) and the index of the session's existing memory entries. Your job has TWO\n"
    "parts, and both are mandatory:\n"
    "\n"
    "1) RECONCILE the existing entries against the group memory. The group memory is the\n"
    "   NEWER truth. Go through the index, find the entries whose subject the material\n"
    "   covers, read them (memory_get), and check whether what they say still holds. If an\n"
    "   entry's fact has changed, either UPDATE it to the correct content or, if it is\n"
    "   simply obsolete, RETIRE it. An entry that no longer matches the material must not\n"
    "   stay as it is.\n"
    "2) FOLD IN the material's durable knowledge: update a related entry, or create a new\n"
    "   one when nothing related exists.\n"
    "\n"
    "Rules:\n"
    "- SEARCH FIRST: use memory_search (POSIX ERE regexes) / memory_list to find related entries, and\n"
    "  memory_get (a list of ids) to read them in full before changing them.\n"
    "- Prefer UPDATE over creating a near-duplicate. memory_update takes a list; for each item pass\n"
    "  the `rev` you read as `expected_rev`; if the tool says an item is stale, re-read and redo it.\n"
    "- RETIRE entries the material shows to be obsolete or wrong: memory_supersede takes a\n"
    "  list of ids and needs no rev.\n"
    "- memory_write a new entry only when nothing related exists, or when the knowledge\n"
    "  genuinely deserves its own entry.\n"
    "- Record knowledge, not activity: decisions, constraints, gotchas, external facts,\n"
    "  exact paths / commands / error text. Never progress logs, todos or transient\n"
    "  conclusions.\n"
    "- Keep entries small: title <= 80 chars, summary <= 240 chars, detail holds the rest.\n"
    "- TIME IS A HINT, NOT A GATE: every history line carries its timestamp and every entry\n"
    "  carries when it was last updated. If the material is NEWER than an entry, reconcile it\n"
    "  (update to the new truth, or retire it); if the material is OLDER than the entry (the\n"
    "  entry was updated afterwards), treat the entry as already current and do NOT regress it.\n"
    "  If they conflict and time cannot settle it, keep both and say so. Timestamps can be\n"
    "  wrong (the host clock may drift); never discard material only because of a timestamp.\n"
    "- You have ONLY the memory tools. Do not try to read files or run commands.\n"
    "\n"
    "If the material changes nothing, say so and change nothing. When you are done, reply\n"
    "with a one-line summary of what you changed.\n";

const char *edc_session_memory_system_prompt(void)
{
    return kCurateSystemPrompt;
}

/* L2's user message: the current entry index + focus.
 *
 * Group memory is NOT in it -- the automatic path's L2 and the fold are joined by a
 * JOIN edge, so the fold's output (the just-landed group memory) is auto-injected by
 * the request assembler into this same user message (see edc_llm_request.c).
 * That makes the material exactly "what the fold really wrote".
 * all_groups = true is manual curation (/session memo compact): it has no such edge,
 * so it gathers EVERY group's memory into the prompt itself. */
static sds build_curate_prompt(struct edc_session *session, struct edc_task_group *group, bool all_groups,
                               const char *focus)
{
    sds out = sdsempty();
    if (all_groups)
    {
        bool any = false;
        struct edc_task_group *g = NULL;
        if (session)
        {
            DL_FOREACH(session->groups, g)
            {
                if (!g->memory || sdslen(g->memory) == 0)
                    continue;
                any = true;
                sds when = edc_datetime_format_min((time_t)g->memory_updated_at);
                out = sdscatprintf(out, "\n### group %s (memory last updated %s)\n%s", edc_id_str(g->group_id),
                                   when, g->memory);
                sdsfree(when);
            }
        }
        if (!any)
            out = sdscat(out, "(no group has memory yet)");
    }
    else
    {
        out = sdscat(out, "## Group memory to curate\n");
        if (group)
        {
            /* The material's own recency: the curator needs it to tell whether the
             * existing entries are newer or older than this group memory. */
            sds when = edc_datetime_format_min((time_t)group->memory_updated_at);
            sds covers = edc_datetime_format_min((time_t)group->memory_covers_to);
            out = sdscatprintf(out, "Group %s — memory last updated %s (covers up to %s).\n",
                               edc_id_str(group->group_id), when, covers);
            sdsfree(when);
            sdsfree(covers);
        }
        out = sdscat(out, "It is **appended at the end of this message** (the fold task's output).\n"
                          "It is the NEWER truth: reconcile the existing entries against it, then fold in\n"
                          "what is new (use the memory tools; search first).\n");
    }

    if (focus && *focus)
        out = sdscatprintf(out, "\n\nAdditional focus: %s", focus);
    out = sdscat(out, "\n\n## Existing session entries\n");

    const struct edc_session_memory *m = session ? session->memo : NULL;
    size_t total = 0;
    /* The whole index (the entry cap bounds it), so the curator can judge relatedness
     * rather than guess from a truncated list. */
    sds index = edc_session_memory_index_lines(m, 0, EDC_SESSION_MEMORY_MAX_ENTRIES + 1, &total);
    if (sdslen(index))
        out = sdscatsds(out, index);
    else
        out = sdscat(out, "(none yet)");
    sdsfree(index);
    out = sdscat(out, "\n\nNow use the memory tools on the material above:\n"
                      "1) RECONCILE -- find the existing entries it covers (memory_search / memory_list),\n"
                      "   read each (memory_get), and update or retire any whose facts it has changed;\n"
                      "2) FOLD IN -- update a related entry, or memory_write a new one when nothing\n"
                      "   related exists.\n");
    return out;
}

struct edc_task *edc_session_memory_start(struct edc_scheduler *sched, struct edc_session *session,
                                          struct edc_task_group *folded_group, struct edc_task *fold_task)
{
    if (!sched || !session || !session->memo)
        return NULL;

    sds prompt = build_curate_prompt(session, folded_group, /*all_groups=*/false, NULL);
    struct edc_task *task =
        edc_scheduler_spawn_system_task_as(sched, session, "curate session memory", EDC_SINK_SESSION_MEMORY);
    if (!task)
    {
        sdsfree(prompt);
        return NULL;
    }
    /* Remember the source group (for sources/logging); it does not participate in the "group folding" test, which reads only GROUP_MEMORY. */
    task->sink_group = folded_group;

    if (!edc_scheduler_attach_input(sched, task, prompt))
    {
        log_error("L2 memo: could not attach the curation prompt — no curation this round");
        edc_task_mark_failed(task); /* failure also releases dependents */
        sdsfree(prompt);
        return NULL;
    }
    sdsfree(prompt);

    /* Chain: L2 ->(JOIN) L1. JOIN, because the material has to come along: its injection
     * splices L1's output (the just-landed group memory) into L2's own input message, so
     * the memory need not be copied into the prompt when folding. (Failure/cancel cascade
     * through JOIN and SEQUENCE alike, so that is no longer a reason to pick one.)
     * Members additionally hang a SEQUENCE on L2 (below): they wait for both. */
    if (fold_task)
        edc_task_add_dependency(task, fold_task, TASK_DEP_JOIN);
    if (folded_group)
    {
        struct edc_task *m;
        LL_FOREACH2(folded_group->members, m, group_next)
        {
            if (edc_task_is_completed(m) || edc_task_is_cancelled(m) || edc_task_has_failed(m))
                continue;
            edc_task_add_dependency(m, task, TASK_DEP_BARRIER);
        }
    }

    log_info("L2 memo start: session=%s from group=%s task=%s entries=%zu", edc_id_str(session->session_id),
             edc_id_str(folded_group ? folded_group->group_id : 0), edc_id_str(task->task_id),
             session->memo->entry_count);
    return task;
}

struct edc_task *edc_session_memory_start_manual(struct edc_scheduler *sched, struct edc_session *session,
                                                  const char *focus)
{
    if (!sched || !session || !session->memo)
        return NULL;
    /* Same gate as the automatic path: one system task per session at a time. */
    if (edc_scheduler_has_pending_system_task(session))
    {
        log_info("L2 memo: manual compaction refused — a system task is already in flight");
        return NULL;
    }

    sds prompt = build_curate_prompt(session, NULL, /*all_groups=*/true, focus);
    struct edc_task *task =
        edc_scheduler_spawn_system_task_as(sched, session, "curate session memory", EDC_SINK_SESSION_MEMORY);
    if (!task)
    {
        sdsfree(prompt);
        return NULL;
    }
    if (!edc_scheduler_attach_input(sched, task, prompt))
    {
        log_error("L2 memo: could not attach the manual curation prompt — nothing started");
        edc_task_mark_failed(task);
        sdsfree(prompt);
        return NULL;
    }
    sdsfree(prompt);
    log_info("L2 memo start (manual): session=%s task=%s entries=%zu", edc_id_str(session->session_id), edc_id_str(task->task_id),
             session->memo->entry_count);
    return task;
}

/* ══════════════════════════════════════════════════════════════
 *  L2 extraction: an idle group's unfolded history
 *
 *  The third L2 write path (the other two: the fold-chained curation and the
 *  manual `/session memo` curation). The fold chain only curates what a fold
 *  actually covered, so a group that never reaches the fold threshold -- or is
 *  never folded by hand -- keeps its committed history outside L2 forever. This
 *  path closes that gap.
 *
 *  Data (persisted): turn->memo_prefix (the extraction watermark, next to
 *  folded_prefix) and the task's memo_kind = EXTRACT + cut (memo_cut_*).
 *  Invariants:
 *    - folded_prefix <= memo_prefix <= turn_len; both writers only raise it (the
 *      fold landing clamps memo_prefix up to folded_prefix, the loader too).
 *    - memo_prefix NEVER feeds request assembly / group history / fold corpus: it
 *      only answers "has this already reached L2".
 *  Material = the raw unfolded history after the watermark + the FULL entry index.
 *  The old group memory is deliberately NOT included: a group that has memory has
 *  been folded, so that knowledge already reached L2 through the fold chain, and
 *  the entry index is the correct "known" reference -- not the group memory.
 * ══════════════════════════════════════════════════════════════ */

/* Distinct from the curator's prompt, which says the material "was just folded" --
 * it was not. Same two-part job and the same rules otherwise. */
static const char kExtractSystemPrompt[] =
    "You are the session-memory curator for the EdgeClaw agent.\n"
    "\n"
    "You are given the raw, UN-FOLDED conversation history of ONE group (appended at the\n"
    "end of your input) and the index of the session's existing memory entries. That\n"
    "history never reached the group memory, so nothing in it is in the session memory\n"
    "yet except what the existing entries already cover. Your job has TWO parts, and both\n"
    "are mandatory:\n"
    "\n"
    "1) RECONCILE the existing entries against the material. The material is the NEWER\n"
    "   truth. Go through the index, find the entries whose subject it covers, read them\n"
    "   (memory_get), and check whether what they say still holds. If an entry's fact has\n"
    "   changed, either UPDATE it to the correct content or, if it is simply obsolete,\n"
    "   RETIRE it. An entry that no longer matches the material must not stay as it is.\n"
    "2) EXTRACT the material's durable knowledge: update a related entry, or create a new\n"
    "   one when nothing related exists.\n"
    "\n"
    "Rules:\n"
    "- SEARCH FIRST: use memory_search (POSIX ERE regexes) / memory_list to find related entries, and\n"
    "  memory_get (a list of ids) to read them in full before changing them.\n"
    "- Prefer UPDATE over creating a near-duplicate. memory_update takes a list; for each item pass\n"
    "  the `rev` you read as `expected_rev`; if the tool says an item is stale, re-read and redo it.\n"
    "- RETIRE entries the material shows to be obsolete or wrong: memory_supersede takes a\n"
    "  list of ids and needs no rev.\n"
    "- memory_write a new entry only when nothing related exists, or when the knowledge\n"
    "  genuinely deserves its own entry.\n"
    "- Record knowledge, not activity: decisions, constraints, gotchas, external facts,\n"
    "  exact paths / commands / error text. Never progress logs, todos or transient\n"
    "  conclusions.\n"
    "- Keep entries small: title <= 80 chars, summary <= 240 chars, detail holds the rest.\n"
    "- TIME IS A HINT, NOT A GATE: every history line carries its timestamp and every entry\n"
    "  carries when it was last updated. If the material is NEWER than an entry, reconcile it\n"
    "  (update to the new truth, or retire it); if the material is OLDER than the entry (the\n"
    "  entry was updated afterwards), treat the entry as already current and do NOT regress it.\n"
    "  If they conflict and time cannot settle it, keep both and say so. Timestamps can be\n"
    "  wrong (the host clock may drift); never discard material only because of a timestamp.\n"
    "- You have ONLY the memory tools. Do not try to read files or run commands.\n"
    "\n"
    "If the material changes nothing, say so and change nothing. When you are done, reply\n"
    "with a one-line summary of what you changed.\n";

const char *edc_session_memory_extract_prompt(void)
{
    return kExtractSystemPrompt;
}

/* ── Selection: the raw un-extracted committed history, oldest first ── */

struct extract_piece
{
    struct edc_task *task;
    struct edc_turn *turn;
    size_t turn_index; /* index in the member's committed turns queue */
    size_t msg_index;  /* absolute index within that turn */
    /* The message itself, owned by this piece: messages live in the store now, so the
     * column holds the copies it was built from and frees them when done. */
    cJSON *msg;
    long long tokens;
};

/* Free the column, including the message copies the pieces own. */
static void extract_free_pieces(struct extract_piece *ps, size_t cnt)
{
    if (!ps)
        return;
    for (size_t i = 0; i < cnt; i++)
        if (ps[i].msg)
            cJSON_Delete(ps[i].msg);
    free(ps);
}

/* Flatten one member's committed turns, from each turn's memo_prefix onwards, into the
 * column. Only committed turns are read -- extraction never touches an in-flight turn
 * (the group is idle, so there is normally none anyway). */
static void extract_push(struct extract_piece **ps, size_t *cnt, size_t *cap, struct edc_task *t, struct edc_turn *turn,
                         size_t turn_index)
{
    size_t len = (size_t)turn->msg_count;
    size_t from = (turn->memo_prefix > 0) ? (size_t)turn->memo_prefix : 0;
    if (from > len)
        from = len;
    for (size_t mi = from; mi < len; mi++)
    {
        cJSON *msg = edc_turn_msg_at(turn, (int)mi);
        if (!msg)
            continue;
        if (*cnt == *cap)
        {
            *cap = *cap ? *cap * 2 : 64;
            *ps = realloc(*ps, *cap * sizeof(**ps));
        }
        struct extract_piece *fp = &(*ps)[(*cnt)++];
        fp->task = t;
        fp->turn = turn;
        fp->turn_index = turn_index;
        fp->msg_index = mi;
        fp->msg = msg;
        fp->tokens = edc_token_estimate_msg(msg);
    }
}

/* Where one extraction round's cut went: the member it falls in, that member's turn
 * index, and how many leading messages of that turn the round covered. Everything
 * earlier in content order is covered whole. */
struct edc_extract_cut
{
    int64_t task_id; /* 0 = none */
    uint32_t turn;
    uint32_t prefix;
};

static size_t extract_collect(struct edc_task *const *members, size_t n, struct extract_piece **out)
{
    struct extract_piece *ps = NULL;
    size_t cnt = 0, cap = 0;
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *t = members[i];
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
            extract_push(&ps, &cnt, &cap, t, t->turns.data[qi], qi);
    }
    *out = ps;
    return cnt;
}

static sds build_extract_prompt(struct edc_session *session, struct edc_task_group *group,
                                struct edc_extract_cut *out_cut)
{
    if (out_cut)
    {
        out_cut->task_id = 0;
        out_cut->turn = 0;
        out_cut->prefix = 0;
    }
    if (!session || !group)
        return NULL;

    size_t n = 0;
    struct edc_task **members = edc_task_group_content_order(group, &n);
    struct extract_piece *ps = NULL;
    size_t cnt = extract_collect(members, n, &ps);
    if (cnt == 0)
    {
        free(members);
        extract_free_pieces(ps, cnt);
        return NULL;
    }

    long long total = 0;
    for (size_t i = 0; i < cnt; i++)
        total += ps[i].tokens;

    /* Corpus cap per round, reusing the fold's: the same "do not push an
     * un-compressible pile onto one call" argument. Over the cap the cut moves back
     * so only the oldest span is sent; the rest waits for the next round (each round
     * advances at least one message, so the loop terminates). Never move the cut to 0
     * (a "due but yields nothing" spin). */
    size_t cut_at = cnt; /* index of the first NOT-covered piece */
    if (total > EDC_FOLD_MAX_CORPUS_TOKENS)
    {
        long long acc = 0;
        cut_at = 0;
        while (cut_at < cnt && acc + ps[cut_at].tokens <= EDC_FOLD_MAX_CORPUS_TOKENS)
        {
            acc += ps[cut_at].tokens;
            cut_at++;
        }
        if (cut_at == 0)
            cut_at = 1;
        log_info("L2 memo extract: group %s corpus %lld > cap %d — extracting the oldest %zu of %zu msgs",
                 edc_id_str(group->group_id), total, EDC_FOLD_MAX_CORPUS_TOKENS, cut_at, cnt);
    }

    if (out_cut)
    {
        if (cut_at < cnt)
        {
            out_cut->task_id = ps[cut_at].task->task_id;
            out_cut->turn = (uint32_t)ps[cut_at].turn_index;
            out_cut->prefix = (uint32_t)ps[cut_at].msg_index; /* = leading covered count */
        }
        else
        {
            /* Everything covered: the cut is just past the last message of the last turn. */
            struct extract_piece *last = &ps[cnt - 1];
            out_cut->task_id = last->task->task_id;
            out_cut->turn = (uint32_t)last->turn_index;
            out_cut->prefix = (uint32_t)last->turn->msg_count;
        }
    }

    sds out = sdscatprintf(sdsempty(),
                           "## Group history to extract from\n"
                           "Group %s. Everything below is raw, un-folded conversation history (tasks in\n"
                           "content order) that has NOT reached the session memory yet.\n\n",
                           edc_id_str(group->group_id));
    if (group->memory && sdslen(group->memory) > 0)
    {
        /* Neither the group memory's text nor its entries are here, but its recency is:
         * it tells the curator whether the existing entries already reflect it. */
        sds when = edc_datetime_format_min((time_t)group->memory_updated_at);
        out = sdscatprintf(out,
                           "This group has a memory last updated %s (not included here); the entries below are the\n"
                           "session's known state.\n\n",
                           when);
        sdsfree(when);
    }
    struct edc_task *cur = NULL;
    for (size_t i = 0; i < cut_at; i++)
    {
        struct edc_task *tk = ps[i].task;
        if (tk != cur)
        {
            cur = tk;
            out = sdscatprintf(out, "\n### [task %s] %s\n", edc_id_str(tk->task_id), tk->description ? tk->description : "");
        }
        cJSON *m = ps[i].msg;
        if (cJSON_GetStringValue(cJSON_GetObjectItem(m, "role")))
        {
            /* One line, with the message's wall-clock time and (for a tool call) its
             * name + arguments -- see edc_turn_render_message. */
            sds line = edc_turn_render_message(m);
            out = sdscatprintf(out, "%s\n", line);
            sdsfree(line);
        }
    }

    out = sdscat(out, "\n## Existing session entries\n");
    size_t total_entries = 0;
    sds index = edc_session_memory_index_lines(session->memo, 0, EDC_SESSION_MEMORY_MAX_ENTRIES + 1, &total_entries);
    if (sdslen(index))
        out = sdscatsds(out, index);
    else
        out = sdscat(out, "(none yet)");
    sdsfree(index);
    out = sdscat(out, "\n\nNow use the memory tools on the material above:\n"
                      "1) RECONCILE -- find the existing entries it covers (memory_search / memory_list),\n"
                      "   read each (memory_get), and update or retire any whose facts it has changed;\n"
                      "2) EXTRACT -- update a related entry, or memory_write a new one when nothing\n"
                      "   related exists.\n");

    free(members);
    extract_free_pieces(ps, cnt);
    return out;
}

/* ── Due / triggering ──────────────────────────────────────── */

size_t edc_task_group_extractable_msg_count(struct edc_task_group *group)
{
    if (!group)
        return 0;
    size_t total = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue;
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
        {
            struct edc_turn *turn = t->turns.data[qi];
            size_t len = (size_t)turn->msg_count;
            size_t from = (turn->memo_prefix > 0) ? (size_t)turn->memo_prefix : 0;
            if (from < len)
                total += len - from;
        }
    }
    return total;
}

/* Parse a message's wall-clock timestamp ("%Y-%m-%d %H:%M:%S", local time, see
 * edc_turn_add_message). 0 when absent or unparseable. sscanf instead of strptime:
 * the latter is POSIX but not declared without an XOPEN macro. */
static time_t parse_wall_clock(const char *s)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return 0;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* Newest committed message timestamp in the group (the last time anything happened). */
static time_t group_last_activity(struct edc_task_group *group)
{
    time_t best = 0;
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue;
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
        {
            struct edc_turn *turn = t->turns.data[qi];
            for (int mi = 0; mi < turn->msg_count; mi++)
            {
                cJSON *m = edc_turn_msg_at(turn, mi);
                if (!m)
                    continue;
                cJSON *ts = cJSON_GetObjectItem(m, "ts");
                if (ts && cJSON_IsString(ts))
                {
                    time_t v = parse_wall_clock(ts->valuestring);
                    if (v > best)
                        best = v;
                }
                cJSON_Delete(m);
            }
        }
    }
    return best;
}

bool edc_session_memory_extract_due(struct edc_task_group *group, const struct edc_config *conf, time_t now)
{
    if (!group || edc_task_group_is_system(group))
        return false;
    if (group->member_count == 0)
        return false;
    /* Failure cap: automatic rounds stop until the group gets new committed content. */
    if (group->memo_extract_failures >= EDC_MEMO_EXTRACT_MAX_FAILURES)
        return false;

    /* Idle: every member (memory sinks aside) is finished -- nobody is running. */
    struct edc_task *t;
    LL_FOREACH2(group->members, t, group_next)
    {
        if (edc_task_is_memory_sink(t))
            continue;
        if (!edc_task_is_finished(t))
            return false;
    }

    if (edc_task_group_extractable_msg_count(group) == 0)
        return false;

    /* Fold has priority: if a fold is due, let it fold; extraction will pick up any
     * remainder afterwards. And nothing else may be in flight for this group. */
    if (edc_group_fold_due(group, conf))
        return false;
    if (edc_group_fold_in_flight(group))
        return false;

    time_t last = group_last_activity(group);
    if (last == 0)
        return false; /* no usable timestamp -- do not guess */
    if (now < last + EDC_MEMO_EXTRACT_IDLE_SECS)
        return false;

    return true;
}

static struct edc_task *extract_start(struct edc_scheduler *sched, struct edc_session *session,
                                      struct edc_task_group *group)
{
    if (!sched || !session || !session->memo || !group)
        return NULL;
    /* One system task per session at a time (the same gate the fold uses). */
    if (edc_scheduler_has_pending_system_task(session))
        return NULL;

    struct edc_extract_cut cut;
    memset(&cut, 0, sizeof(cut));
    sds prompt = build_extract_prompt(session, group, &cut);
    if (!prompt)
        return NULL;

    struct edc_task *task = edc_scheduler_spawn_system_task_as(sched, session, "extract session memory",
                                                              EDC_SINK_SESSION_MEMORY);
    if (!task)
    {
        sdsfree(prompt);
        return NULL;
    }
    task->memo_kind = EDC_MEMO_KIND_EXTRACT;
    task->sink_group = group;
    task->memo_cut_task_id = cut.task_id;
    task->memo_cut_turn = cut.turn;
    task->memo_cut_prefix = cut.prefix;

    /* No barrier: the group is idle (no non-terminal member to hold), and the round
     * only reads committed turns and raises a watermark -- it changes nothing about
     * what any request sends. */
    if (!edc_scheduler_attach_input(sched, task, prompt))
    {
        log_error("L2 memo extract: could not attach the prompt for group %s — nothing started", edc_id_str(group->group_id));
        edc_task_mark_failed(task); /* failure also releases dependents */
        sdsfree(prompt);
        edc_session_memory_extract_note_failure(group);
        return NULL;
    }
    sdsfree(prompt);
    log_info("L2 memo extract start: session=%s group=%s task=%s cut=task %s turn %u msg %u", edc_id_str(session->session_id),
             edc_id_str(group->group_id), edc_id_str(task->task_id), edc_id_str(task->memo_cut_task_id),
             task->memo_cut_turn, task->memo_cut_prefix);
    return task;
}

bool edc_session_memory_extract_try(struct edc_scheduler *sched, struct edc_session_manager *sessions,
                                    struct edc_config *conf, struct edc_session **out_session)
{
    if (out_session)
        *out_session = NULL;
    if (!sched || !sessions)
        return false;

    time_t now = time(NULL);
    struct edc_session_manager *sm = sessions;
    for (int si = 0; si < sm->session_count; si++)
    {
        struct edc_session *sess = sm->sessions[si];
        if (!sess || !sess->memo)
            continue;
        if (edc_scheduler_has_pending_system_task(sess))
            continue;

        struct edc_task_group *g;
        DL_FOREACH(sess->groups, g)
        {
            if (!edc_session_memory_extract_due(g, conf, now))
                continue;
            log_info("L2 memo extract trigger: group %s idle, %zu msgs pending", edc_id_str(g->group_id),
                     edc_task_group_extractable_msg_count(g));
            struct edc_task *task = extract_start(sched, sess, g);
            if (task)
            {
                if (out_session)
                    *out_session = sess;
                return true; /* one group at a time */
            }
        }
    }
    return false;
}

void edc_session_memory_extract_note_failure(struct edc_task_group *group)
{
    if (!group)
        return;
    group->memo_extract_failures++;
    if (group->memo_extract_failures >= EDC_MEMO_EXTRACT_MAX_FAILURES)
        log_error("L2 memo extract: group %s failed %d times in a row — automatic extraction is paused for this "
                  "group (do new work in it, or fold it, to try again)",
                  edc_id_str(group->group_id), group->memo_extract_failures);
    else
        log_info("L2 memo extract: group %s failure %d/%d", edc_id_str(group->group_id), group->memo_extract_failures,
                 EDC_MEMO_EXTRACT_MAX_FAILURES);
}

void edc_session_memory_extract_note_progress(struct edc_task_group *group)
{
    if (!group || group->memo_extract_failures == 0)
        return;
    log_info("L2 memo extract: group %s failure streak cleared (was %d)", edc_id_str(group->group_id),
             group->memo_extract_failures);
    group->memo_extract_failures = 0;
}

/* ── Landing: advance the watermark by the recorded cut ── */

static void extract_turn_whole(struct edc_turn *turn)
{
    size_t len = (size_t)turn->msg_count;
    if ((size_t)turn->memo_prefix < len)
        turn->memo_prefix = (int)len;
}

static void extract_turn_prefix(struct edc_turn *turn, uint32_t prefix)
{
    size_t len = (size_t)turn->msg_count;
    size_t want = prefix;
    if (want > len)
        want = len;
    if ((size_t)turn->memo_prefix < want)
        turn->memo_prefix = (int)want;
}

bool edc_session_memory_extract_land(struct edc_task *task)
{
    struct edc_task_group *group = task ? task->sink_group : NULL;
    if (!group || !task->memo_cut_task_id)
    {
        log_error("L2 memo extract: task %s carries no resolvable cut — watermark unchanged",
                  edc_id_str(task ? task->task_id : 0));
        edc_session_memory_extract_note_failure(group);
        return false;
    }

    size_t n = 0;
    struct edc_task **members = edc_task_group_content_order(group, &n);
    struct edc_task *cut_member = NULL;
    for (size_t i = 0; i < n && !cut_member; i++)
    {
        if (task->memo_cut_task_id > 0 && members[i]->task_id == task->memo_cut_task_id)
            cut_member = members[i];
    }

    struct edc_turn *cut_turn = NULL;
    if (cut_member && (size_t)task->memo_cut_turn >= cut_member->turns.head &&
        (size_t)task->memo_cut_turn < cut_member->turns.tail)
        cut_turn = cut_member->turns.data[(size_t)task->memo_cut_turn];

    if (!cut_member || !cut_turn)
    {
        log_error("L2 memo extract: cut (task=%s turn=%u) does not resolve in group %s — watermark unchanged",
                  edc_id_str(task->memo_cut_task_id), task->memo_cut_turn, edc_id_str(group->group_id));
        free(members);
        edc_session_memory_extract_note_failure(group);
        return false;
    }

    /* Members before the cut: every committed turn whole. The cut's member: turns
     * before the cut turn whole, the cut turn up to its prefix. Nothing after. */
    size_t advanced_turns = 0;
    for (size_t i = 0; i < n; i++)
    {
        struct edc_task *t = members[i];
        bool stop_after = false;
        for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
        {
            struct edc_turn *turn = t->turns.data[qi];
            if (turn == cut_turn)
            {
                extract_turn_prefix(turn, task->memo_cut_prefix);
                stop_after = true;
                break;
            }
            extract_turn_whole(turn);
            advanced_turns++;
        }
        if (t == cut_member)
            break;
    }
    free(members);

    edc_session_memory_extract_note_progress(group);
    log_info("L2 memo extract done: group %s advanced %zu turns, cut at task %s turn %u msg %u", edc_id_str(group->group_id),
             advanced_turns, edc_id_str(task->memo_cut_task_id), task->memo_cut_turn, task->memo_cut_prefix);
    return true;
}

/* ── `/session memo` text ──────────────────────────────────── */

sds edc_session_memory_show_text(const struct edc_session_memory *m, const char *id)
{
    if (!m)
        return sdsnew("**Session memory** — no session loaded.");

    if (id && *id)
    {
        sds detail = edc_session_memory_get_detail(m, id);
        if (!detail)
            return sdscatprintf(sdsempty(), "**Session memory** — no entry `%s`.", id);
        struct edc_memo_entry *e = NULL;
        for (size_t i = 0; i < m->entry_count; i++)
        {
            if (m->entries[i].id && strcmp(m->entries[i].id, id) == 0)
                e = &m->entries[i];
        }
        sds out = sdscatprintf(sdsempty(), "**Session memory · %s**%s\n\n", id,
                               (e && e->superseded) ? " (retired)" : "");
        if (e && e->title)
            out = sdscatprintf(out, "**%s**\n\n", e->title);
        out = sdscatsds(out, detail);
        if (e && e->sources)
            out = sdscatprintf(out, "\n\n_sources: %s_", e->sources);
        if (e && e->proposed_by)
            out = sdscatprintf(out, "  _proposed by task %s_", e->proposed_by);
        sdsfree(detail);
        return out;
    }

    size_t total = 0;
    sds lines = edc_session_memory_index_lines(m, 0, 200, &total);
    if (total == 0)
        return sdsnew("**Session memory** — nothing yet. It fills up when a group is folded "
                      "(`/group fold`) or a task records something (`memory_write`).");
    sds out = sdscatprintf(sdsempty(), "**Session memory** — %zu entr%s\n\n", total, total == 1 ? "y" : "ies");
    out = sdscatsds(out, lines);
    out = sdscat(out, "\n\n`/session memo show <id>` for one entry · `/session memo search <q>` · "
                      "`/session memo compact` to curate now");
    sdsfree(lines);
    return out;
}

sds edc_session_memory_search_text(const struct edc_session_memory *m, const char *query)
{
    if (!m || !query || !*query)
        return sdsnew("Usage: `/session memo search <keyword>`");
    size_t hits = 0;
    sds lines = edc_session_memory_search_lines(m, query, 50, &hits);
    if (hits == 0)
    {
        sdsfree(lines);
        return sdscatprintf(sdsempty(), "**Session memory** — nothing matches `%s`.", query);
    }
    sds out = sdscatprintf(sdsempty(), "**Session memory** — %zu hit%s for `%s`\n\n", hits, hits == 1 ? "" : "s",
                           query);
    out = sdscatsds(out, lines);
    sdsfree(lines);
    return out;
}

/* ── In-place mutation: update / supersede ──────────────────
 * The two write primitives behind the memory_update / memory_supersede tools (and
 * behind the curation task, which uses those tools directly):
 * Both append an event, so other tasks see the
 * change on their next request; nothing is ever deleted (supersede is a tombstone). */

enum edc_memo_write_result edc_session_memory_update(struct edc_session_memory *m, const char *id, const char *title,
                                                     const char *summary, const char *detail, uint64_t expected_rev,
                                                     uint64_t *out_rev)
{
    if (out_rev)
        *out_rev = 0;
    struct edc_memo_entry *e = edc_session_memory_entry(m, id);
    if (!e)
        return EDC_MEMO_WRITE_NOT_FOUND;
    /* Optimistic concurrency: refuse to overwrite content the caller did not read.
     * expected_rev is required and always a real rev (>= 1), so it can never match a
     * sentinel. Nothing changes on conflict. */
    if (expected_rev != e->updated_seq)
    {
        if (out_rev)
            *out_rev = e->updated_seq;
        return EDC_MEMO_WRITE_CONFLICT;
    }
    /* Given fields overwrite; missing ones stay. */
    if (title)
    {
        free(e->title);
        e->title = strdup(title);
    }
    if (summary)
    {
        free(e->summary);
        e->summary = strdup(summary);
    }
    if (detail)
    {
        if (e->detail)
            sdsfree(e->detail);
        e->detail = sdsnew(detail);
    }
    uint64_t seq = edc_session_memory_add_event(m, id, EDC_MEMO_UPDATED, e->title, e->summary);
    e->updated_seq = seq;
    e->updated_at = (uint64_t)edc_datetime_now();
    if (out_rev)
        *out_rev = seq;
    memo_trim_if_due(m);
    return EDC_MEMO_WRITE_OK;
}

bool edc_session_memory_supersede(struct edc_session_memory *m, const char *const *ids, size_t n)
{
    if (!m || !ids || n == 0)
        return false;
    /* Atomic: every id must resolve before anything changes. */
    for (size_t i = 0; i < n; i++)
    {
        if (!ids[i] || !edc_session_memory_entry(m, ids[i]))
            return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        struct edc_memo_entry *e = edc_session_memory_entry(m, ids[i]);
        if (e->superseded)
            continue; /* idempotent: already retired, no duplicate event */
        e->superseded = true;
        uint64_t seq = edc_session_memory_add_event(m, ids[i], EDC_MEMO_SUPERSEDED, e->title, e->summary);
        e->updated_seq = seq;
        e->updated_at = (uint64_t)edc_datetime_now();
    }
    memo_trim_if_due(m);
    return true;
}

/* ── Delete / compaction ───────────────────────────────────── */

bool edc_session_memory_delete(struct edc_session_memory *m, const char *const *ids, size_t n)
{
    if (!m || !ids || n == 0)
        return false;
    /* Atomic: every id must resolve before anything is removed. */
    for (size_t i = 0; i < n; i++)
    {
        if (!ids[i] || !edc_session_memory_entry(m, ids[i]))
            return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        struct edc_memo_entry *e = edc_session_memory_entry(m, ids[i]);
        /* Announce before removing: the entry is about to be gone for good, and a task that
         * already saw it has to be told (the event carries no content). */
        edc_session_memory_add_event(m, ids[i], EDC_MEMO_DELETED, e->title, e->summary);
        size_t idx = (size_t)(e - m->entries);
        memo_entry_free(&m->entries[idx]);
        memmove(&m->entries[idx], &m->entries[idx + 1], (m->entry_count - idx - 1) * sizeof(*m->entries));
        m->entry_count--;
    }
    return true;
}

/* Trim the event log to the newest EDC_SESSION_MEMORY_KEEP_EVENTS: free the oldest, advance
 * `pruned_before` to the highest seq dropped, memmove the rest down. Returns how many events
 * went away.
 *
 * Safe because the log is a change FEED, not the knowledge: the notice says "N more" and
 * every reader can fall back to memory_list / memory_search for the current state (that is
 * what those tools are for). */
size_t edc_session_memory_trim_events(struct edc_session_memory *m)
{
    if (!m || m->event_count <= EDC_SESSION_MEMORY_KEEP_EVENTS)
        return 0;

    size_t drop = m->event_count - EDC_SESSION_MEMORY_KEEP_EVENTS;
    for (size_t i = 0; i < drop; i++)
    {
        if (m->events[i].seq > m->pruned_before)
            m->pruned_before = m->events[i].seq;
        memo_event_free(&m->events[i]);
    }
    memmove(m->events, m->events + drop, (m->event_count - drop) * sizeof(*m->events));
    m->event_count -= drop;
    return drop;
}

/* The write path's automatic trigger: "periodic" here means a threshold at a natural
 * boundary (every write already appends an event), not a timer -- the same choice the rest
 * of the runtime makes. */
static void memo_trim_if_due(struct edc_session_memory *m)
{
    if (!m || m->event_count <= EDC_SESSION_MEMORY_TRIM_EVENTS)
        return;
    size_t dropped = edc_session_memory_trim_events(m);
    log_info("session memory: trimmed %zu old events, %zu kept", dropped, m->event_count);
}

/* ── Tail notice ───────────────────────────────────────────── */

sds edc_session_memory_notice(const struct edc_session_memory *m, uint64_t seen, size_t cap, uint64_t *out_max_injected)
{
    if (out_max_injected)
        *out_max_injected = 0;
    if (!m || m->next_seq <= seen || m->event_count == 0)
        return NULL;

    /* Keep only the newest event per id (the model wants the current state, not a log). */
    const struct edc_memo_event **picked = malloc(m->event_count * sizeof(*picked));
    if (!picked)
        return NULL;
    size_t npick = 0;
    for (size_t i = 0; i < m->event_count; i++)
    {
        const struct edc_memo_event *e = &m->events[i];
        if (e->seq <= seen || !e->entry_id)
            continue;
        size_t j = 0;
        for (; j < npick; j++)
        {
            if (picked[j]->entry_id && strcmp(picked[j]->entry_id, e->entry_id) == 0)
            {
                if (e->seq > picked[j]->seq) /* replace only if newer */
                    picked[j] = e;
                break;
            }
        }
        if (j == npick)
            picked[npick++] = e;
    }
    if (npick == 0)
    {
        free(picked);
        return NULL;
    }

    /* Sort ascending by seq (pick is "first seen" order, not monotonic), then take the NEWEST cap. */
    for (size_t i = 1; i < npick; i++)
    {
        const struct edc_memo_event *cur = picked[i];
        size_t j = i;
        while (j > 0 && picked[j - 1]->seq > cur->seq)
        {
            picked[j] = picked[j - 1];
            j--;
        }
        picked[j] = cur;
    }

    /* The newest cap (overflow is reported as a count). */
    size_t first = (npick > cap) ? (npick - cap) : 0;
    size_t shown = npick - first;

    sds body = sdsempty();
    for (size_t i = first; i < npick; i++)
    {
        const struct edc_memo_event *ev = picked[i];
        const struct edc_memo_entry *en = NULL;
        for (size_t k = 0; k < m->entry_count; k++)
        {
            if (m->entries[k].id && strcmp(m->entries[k].id, ev->entry_id) == 0)
            {
                en = &m->entries[k];
                break;
            }
        }
        if (sdslen(body))
            body = sdscat(body, "\n");
        sds when = edc_datetime_format_min((time_t)ev->ts);
        if (ev->kind == EDC_MEMO_DELETED)
        {
            /* A deletion is a CHANGE, not knowledge: announce it without the content
             * (the entry has left every view). */
            body = sdscatprintf(body, "- [%s] deleted (%s)", ev->entry_id, when);
            sdsfree(when);
            continue;
        }
        body = sdscatprintf(body, "- [%s] %s: %s", ev->entry_id, edc_memo_event_kind_name(ev->kind),
                            en && en->title ? en->title : (ev->title ? ev->title : "(untitled)"));
        const char *sum = (en && en->summary && en->summary[0]) ? en->summary : ev->summary;
        if (sum && sum[0])
            body = sdscatprintf(body, " — %s", sum);
        /* When the change happened, so a reader can weigh recency against what it knows. */
        body = sdscatprintf(body, " (%s)", when);
        sdsfree(when);
    }

    sds out = sdscatprintf(sdsempty(), "## Session Memory Updates  (共 %zu 条)\n", m->entry_count);
    if (seen < m->pruned_before)
        out = sdscatprintf(out, "[events up to seq %llu were compacted away — the index/search hold the full picture]\n",
                           (unsigned long long)m->pruned_before);
    out = sdscatsds(out, body);
    sdsfree(body); /* copied into out: the per-event listing is not needed past this point */
    if (npick > shown)
        out = sdscatprintf(out, "\n… %zu more（memory_search <patterns> / memory_list [offset] [n] / memory_get <ids>）",
                           npick - shown);
    else
        out = sdscat(out, "\n（memory_list / memory_search / memory_get 可查完整内容）");
    /* Ride the reconcile/merge/create discipline along with every notice (same rule as the
     * static system-prompt hint) -- the notice is exactly the moment memory is in front of
     * the model, so it must not default to appending. */
    out = sdscat(out,
                 "\n\nWhen you act on memory, do NOT just append: RECONCILE first (memory_search / memory_get; "
                 "memory_update an entry that is now wrong, memory_supersede one that is obsolete), then MERGE "
                 "into a related entry; memory_write only when nothing related exists. memory_update needs the "
                 "entry's `rev` as expected_rev.");

    free(picked);
    /* The watermark advances to the HEADER: the "newest cap" window guarantees the
     * newest is included, and cap-truncated old events are covered by the notice's
     * "N more" + memory_list/memory_search (see the header). */
    if (out_max_injected)
        *out_max_injected = m->next_seq;
    return out;
}

/* ── Birth-point adoption / proposer ───────────────────────── */

void edc_session_memory_adopt_task(struct edc_session *session, struct edc_task *task)
{
    if (!session || !task || !session->memo)
        return;
    /* 0 = "has seen nothing" => the first request carries a backfill notice (newest cap). */
    task->memo_seen_seq = 0;
}

void edc_session_memory_note_proposer(struct edc_memo_entry *entry, const char *task_id)
{
    if (!entry)
        return;
    free(entry->proposed_by);
    entry->proposed_by = dup_or_null(task_id);
}

/* ── Persistence / load ────────────────────────────────────── */

static void json_add_str(cJSON *o, const char *key, const char *v)
{
    if (v)
        cJSON_AddStringToObject(o, key, v);
}

cJSON *edc_session_memory_to_json(const struct edc_session_memory *m)
{
    cJSON *root = cJSON_CreateObject();
    if (!m)
        return root; /* write even an empty structure so the loader need not handle a missing key */

    cJSON_AddNumberToObject(root, "next_seq", (double)m->next_seq);
    cJSON_AddNumberToObject(root, "next_entry_id", (double)m->next_entry_id);
    cJSON_AddNumberToObject(root, "pruned_before", (double)m->pruned_before);

    cJSON *events = cJSON_AddArrayToObject(root, "events");
    for (size_t i = 0; i < m->event_count; i++)
    {
        const struct edc_memo_event *e = &m->events[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", (double)e->seq);
        cJSON_AddNumberToObject(o, "ts", (double)e->ts);
        json_add_str(o, "entry_id", e->entry_id);
        cJSON_AddStringToObject(o, "kind", edc_memo_event_kind_name(e->kind));
        json_add_str(o, "title", e->title);
        json_add_str(o, "summary", e->summary);
        cJSON_AddItemToArray(events, o);
    }

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        cJSON *o = cJSON_CreateObject();
        json_add_str(o, "id", e->id);
        json_add_str(o, "title", e->title);
        json_add_str(o, "summary", e->summary);
        if (e->detail)
            cJSON_AddStringToObject(o, "detail", e->detail);
        json_add_str(o, "sources", e->sources);
        json_add_str(o, "proposed_by", e->proposed_by);
        cJSON_AddNumberToObject(o, "created_seq", (double)e->created_seq);
        cJSON_AddNumberToObject(o, "updated_seq", (double)e->updated_seq);
        cJSON_AddNumberToObject(o, "created_at", (double)e->created_at);
        cJSON_AddNumberToObject(o, "updated_at", (double)e->updated_at);
        cJSON_AddBoolToObject(o, "superseded", e->superseded);
        cJSON_AddItemToArray(entries, o);
    }

    return root;
}

static const char *json_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

struct edc_session_memory *edc_session_memory_from_json(const cJSON *json)
{
    struct edc_session_memory *m = edc_session_memory_new();
    if (!m)
        return NULL;
    if (!json || !cJSON_IsObject(json))
        return m;

    m->next_seq = (uint64_t)json_num(json, "next_seq");
    m->next_entry_id = (uint64_t)json_num(json, "next_entry_id");
    m->pruned_before = (uint64_t)json_num(json, "pruned_before");

    /* Events: backfill with the on-disk seq (the writer guarantees monotonicity; next_seq rises to the max). */
    const cJSON *ev;
    cJSON_ArrayForEach(ev, cJSON_GetObjectItemCaseSensitive(json, "events"))
    {
        const char *eid = json_str(ev, "entry_id");
        if (!eid)
            continue;
        uint64_t seq = (uint64_t)json_num(ev, "seq");
        if (!append_event(m, seq, (uint64_t)json_num(ev, "ts"), eid, edc_memo_event_classify_kind(json_str(ev, "kind")),
                          json_str(ev, "title"), json_str(ev, "summary")))
            continue;
        if (seq > m->next_seq)
            m->next_seq = seq;
    }

    const cJSON *en;
    cJSON_ArrayForEach(en, cJSON_GetObjectItemCaseSensitive(json, "entries"))
    {
        const char *id = json_str(en, "id");
        if (!id)
            continue;
        /* Compatibility with the short-lived logical-delete marker: an archive written by a
         * build where `deleted` meant "hidden but kept" is honoured by DROPPING the row at
         * load. Without this, those rows would silently come back the next time the memory
         * is read (the field is gone from the model now). */
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(en, "deleted")))
        {
            if ((uint64_t)entry_id_num(id) > m->next_entry_id)
                m->next_entry_id = (uint64_t)entry_id_num(id);
            continue;
        }
        struct edc_memo_entry *e = edc_session_memory_upsert_entry(m, id, json_str(en, "title"),
                                                                   json_str(en, "summary"), json_str(en, "detail"),
                                                                   json_str(en, "sources"));
        if (!e)
            continue;
        edc_session_memory_note_proposer(e, json_str(en, "proposed_by"));
        /* Old (or hand-written) archives missing the key can still allocate: raise above the largest id seen. */
        if ((uint64_t)entry_id_num(id) > m->next_entry_id)
            m->next_entry_id = (uint64_t)entry_id_num(id);
        e->created_seq = (uint64_t)json_num(en, "created_seq");
        e->updated_seq = (uint64_t)json_num(en, "updated_seq");
        e->created_at = (uint64_t)json_num(en, "created_at");
        e->updated_at = (uint64_t)json_num(en, "updated_at");
        const cJSON *sup = cJSON_GetObjectItemCaseSensitive(en, "superseded");
        e->superseded = cJSON_IsTrue(sup) != 0;
    }

    return m;
}
