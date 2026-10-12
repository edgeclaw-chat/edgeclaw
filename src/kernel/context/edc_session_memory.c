#include "edc_session_memory.h"

#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"

#include "edc_compat.h"
#include "log.h"
#include "utlist_fix.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

/* ── Event table ───────────────────────────────────────────── */

/* Append an event with a PREDETERMINED seq (an assembly internal: the writer supplies seq, load uses the on-disk seq). */
static struct edc_memo_event *append_event(struct edc_session_memory *m, uint64_t seq, const char *entry_id,
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
    e->entry_id = dup_or_null(entry_id);
    e->kind = kind;
    e->title = dup_or_null(title);
    e->summary = dup_or_null(summary);
    m->event_count++;
    return e;
}

uint64_t edc_session_memory_add_event(struct edc_session_memory *m, const char *entry_id,
                                      enum edc_memo_event_kind kind, const char *title, const char *summary)
{
    if (!m)
        return 0;
    uint64_t seq = m->next_seq + 1;
    if (!append_event(m, seq, entry_id, kind, title, summary))
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
    return line;
}

sds edc_session_memory_index_lines(const struct edc_session_memory *m, size_t offset, size_t n, size_t *out_total)
{
    if (out_total)
        *out_total = m ? m->entry_count : 0;
    if (!m || n == 0)
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

sds edc_session_memory_get_detail(const struct edc_session_memory *m, const char *id)
{
    if (!m || !id)
        return NULL;
    for (size_t i = 0; i < m->entry_count; i++)
    {
        const struct edc_memo_entry *e = &m->entries[i];
        if (e->id && strcmp(e->id, id) == 0)
            return (e->detail && sdslen(e->detail) > 0) ? sdsdup(e->detail) : index_line_for(e);
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
    return id;
}

/* ── L2 curation task ──────────────────────────────────────── */

/* Curation instruction: same family as the fold instruction -- the system section of
 * a self-contained request, constant text (so the L2 call's prefix is stable). The
 * output contract is the six-section summary. */
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
    "- SEARCH FIRST: use memory_search / memory_list to find related entries, and memory_get\n"
    "  (a list of ids) to read them in full before changing them.\n"
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
static sds build_curate_prompt(struct edc_session *session, bool all_groups, const char *focus)
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
                out = sdscatprintf(out, "\n### group %s\n%s", g->group_id ? g->group_id : "?", g->memory);
            }
        }
        if (!any)
            out = sdscat(out, "(no group has memory yet)");
    }
    else
    {
        out = sdscat(out, "## Group memory to curate\n"
                          "It is **appended at the end of this message** (the fold task's output).\n"
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

    sds prompt = build_curate_prompt(session, /*all_groups=*/false, NULL);
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

    /* Chain: L2 ->(JOIN) L1 -- JOIN rather than SEQUENCE, for two reasons:
     *   1) failure/cancel cascade automatically: L1 ERROR => L2 ERROR; L1 cancelled
     *      => L2 cancelled (SEQUENCE would just drop the edge and let L2 run on stale
     *      memory for nothing);
     *   2) the material comes along: JOIN's injection splices L1's output (the
     *      just-landed group memory) into L2's own input message, so the memory need
     *      not be copied into the prompt when folding.
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
            edc_task_add_dependency(m, task, TASK_DEP_SEQUENCE);
        }
    }

    log_info("L2 memo start: session=%s from group=%s task=%s entries=%zu", session->session_id,
             (folded_group && folded_group->group_id) ? folded_group->group_id : "?", task->task_id,
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

    sds prompt = build_curate_prompt(session, /*all_groups=*/true, focus);
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
    log_info("L2 memo start (manual): session=%s task=%s entries=%zu", session->session_id, task->task_id,
             session->memo->entry_count);
    return task;
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
    if (out_rev)
        *out_rev = seq;
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
    }
    return true;
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
        body = sdscatprintf(body, "- [%s] %s: %s", ev->entry_id, edc_memo_event_kind_name(ev->kind),
                            en && en->title ? en->title : (ev->title ? ev->title : "(untitled)"));
        const char *sum = (en && en->summary && en->summary[0]) ? en->summary : ev->summary;
        if (sum && sum[0])
            body = sdscatprintf(body, " — %s", sum);
    }

    sds out = sdscatprintf(sdsempty(), "## Session Memory Updates  (共 %zu 条)\n%s", m->entry_count, body);
    if (npick > shown)
        out = sdscatprintf(out, "\n… %zu more（memory_list 0 20 / memory_search <关键词> / memory_get <id>）",
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
        if (!append_event(m, seq, eid, edc_memo_event_classify_kind(json_str(ev, "kind")), json_str(ev, "title"),
                          json_str(ev, "summary")))
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
        const cJSON *sup = cJSON_GetObjectItemCaseSensitive(en, "superseded");
        e->superseded = cJSON_IsTrue(sup) != 0;
    }

    return m;
}
