#include "edc_tools_memory.h"

#include "edc_compat.h"
#include "edc_error.h"
#include "edc_param_types.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "sds.h"
#include <cJSON.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The four tools' shared precondition: calling task -> its owning session's session
 * memory. A non-task path (ctx->task == NULL) or a session with no memory = error, do
 * not guess. */
static struct edc_session_memory *memory_of(struct edc_toolbox_call_ctx *ctx, sds *err_out)
{
    if (!ctx || !ctx->task || !ctx->task->group || !ctx->task->group->session ||
        !ctx->task->group->session->memo)
    {
        if (err_out)
            *err_out = sdsnew("Error: session memory is not reachable from this call (no owning session).");
        return NULL;
    }
    return ctx->task->group->session->memo;
}

/* Which group an entry written by this call is sourced from: a curation task carries
 * the just-folded group in sink_group; any other task is sourced from its own group. */
static const char *memo_sources_of(struct edc_toolbox_call_ctx *ctx)
{
    struct edc_task *t = ctx ? ctx->task : NULL;
    if (!t)
        return NULL;
    if (t->sink_group && t->sink_group->group_id)
        return t->sink_group->group_id;
    if (t->group && t->group->group_id)
        return t->group->group_id;
    return NULL;
}

/* Non-blocking duplicate nudge for memory_write: index lines of EXISTING entries that
 * look related to what is about to be written (substring match on the title, falling
 * back to the summary). Called BEFORE the create so the new entry cannot name itself.
 * Returns NULL when nothing looks related; caller sdsfree. It never blocks the write --
 * the model still decides merge-vs-create. */
static sds memo_related_hint(struct edc_session_memory *m, const char *title, const char *summary)
{
    size_t hits = 0;
    sds lines = NULL;
    if (title && *title)
        lines = edc_session_memory_search_lines(m, title, 3, &hits);
    if ((!lines || sdslen(lines) == 0) && summary && *summary)
    {
        if (lines)
            sdsfree(lines);
        lines = edc_session_memory_search_lines(m, summary, 3, &hits);
    }
    if (lines && sdslen(lines) == 0)
    {
        sdsfree(lines);
        lines = NULL;
    }
    return lines;
}

static void fail(struct edc_toolbox_call_ctx *ctx, void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *),
                 const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sds msg = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);
    EdcError *err = edc_error_new(EDC_ERR_INVAL, "%s", msg);
    callback(ctx, msg, err);
}

static int json_int(cJSON *args, const char *key, int def)
{
    cJSON *v = cJSON_GetObjectItem(args, key);
    if (!v || !cJSON_IsNumber(v))
        return def;
    return (int)cJSON_GetNumberValue(v);
}

/* Read a required unsigned 64-bit field (revs are small; exact below 2^53). */
static bool json_u64(cJSON *args, const char *key, uint64_t *out)
{
    cJSON *v = cJSON_GetObjectItem(args, key);
    if (!v || !cJSON_IsNumber(v))
        return false;
    *out = (uint64_t)v->valuedouble;
    return true;
}

/* Conflict reply for a guarded write: say what changed and show the CURRENT content, so
 * the model can re-read and redo the merge in one step. Caller sdsfree. */
static sds memo_conflict_msg(struct edc_session_memory *m, const char *id, uint64_t expected_rev, uint64_t now_rev)
{
    sds cur = edc_session_memory_get_detail(m, id);
    if (cur && sdslen(cur) > 1200)
    {
        sdsrange(cur, 0, 1199);
        cur = sdscat(cur, "…");
    }
    sds out = sdscatprintf(sdsempty(),
                           "Error: `%s` changed since you read it (rev %llu -> %llu) — refused, nothing changed.\n"
                           "Current content:\n%s\n\nRe-read it (memory_get) and redo the merge, then pass the new "
                           "rev (%llu).",
                           id, (unsigned long long)expected_rev, (unsigned long long)now_rev,
                           cur ? cur : "(empty)", (unsigned long long)now_rev);
    if (cur)
        sdsfree(cur);
    return out;
}

/* ── memory_search ─────────────────────────────────────────── */

static void memory_search_exec(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                               void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    sds err = NULL;
    struct edc_session_memory *m = memory_of(ctx, &err);
    if (!m)
    {
        callback(ctx, err, edc_error_new(EDC_ERR_INVAL, "%s", err));
        return;
    }

    const char *query = cJSON_GetStringValue(cJSON_GetObjectItem(args, "query"));
    if (!query || !query[0])
    {
        fail(ctx, callback, "Error: `query` is required.");
        return;
    }

    size_t hits = 0;
    sds lines = edc_session_memory_search_lines(m, query, 20, &hits);
    sds out = sdscatprintf(sdsempty(), "__MEMORY_SEARCH__ %zu hit(s) for \"%s\"\n%s", hits, query,
                           sdslen(lines) ? lines : "(no match)");
    sdsfree(lines);
    callback(ctx, out, NULL);
}

/* ── memory_list ───────────────────────────────────────────── */

static void memory_list_exec(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                             void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    sds err = NULL;
    struct edc_session_memory *m = memory_of(ctx, &err);
    if (!m)
    {
        callback(ctx, err, edc_error_new(EDC_ERR_INVAL, "%s", err));
        return;
    }

    int offset = json_int(args, "offset", 0);
    int n = json_int(args, "n", 20);
    if (offset < 0)
        offset = 0;
    if (n <= 0)
        n = 20;
    if (n > 50)
        n = 50;

    size_t total = 0;
    sds lines = edc_session_memory_index_lines(m, (size_t)offset, (size_t)n, &total);
    sds out = sdscatprintf(sdsempty(), "__MEMORY_LIST__ %d..%d of %zu\n%s", offset, offset + n, total,
                           sdslen(lines) ? lines : "(nothing at this offset)");
    sdsfree(lines);
    callback(ctx, out, NULL);
}

/* ── memory_get ────────────────────────────────────────────── */

static void memory_get_exec(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                            void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    sds err = NULL;
    struct edc_session_memory *m = memory_of(ctx, &err);
    if (!m)
    {
        callback(ctx, err, edc_error_new(EDC_ERR_INVAL, "%s", err));
        return;
    }

    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(args, "id"));
    if (!id || !id[0])
    {
        fail(ctx, callback, "Error: `id` is required (e.g. \"m12\"; get ids from memory_list / memory_search).");
        return;
    }

    sds detail = edc_session_memory_get_detail(m, id);
    if (!detail)
    {
        fail(ctx, callback, "Error: no memory entry with id \"%s\" (ids are stable; list them with memory_list).", id);
        return;
    }
    /* Surface the entry's rev: memory_update / memory_supersede require it as
     * expected_rev, so a write built on this read cannot clobber a newer edit. */
    struct edc_memo_entry *e = edc_session_memory_entry(m, id);
    uint64_t rev = e ? e->updated_seq : 0;
    sds out = sdscatprintf(sdsempty(), "rev: %llu\n\n%s", (unsigned long long)rev, detail);
    sdsfree(detail);
    callback(ctx, out, NULL);
}

/* ── memory_write (foreground direct write) ────────────────── */

static void memory_write_exec(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                              void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    sds err = NULL;
    struct edc_session_memory *m = memory_of(ctx, &err);
    if (!m)
    {
        callback(ctx, err, edc_error_new(EDC_ERR_INVAL, "%s", err));
        return;
    }

    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(args, "title"));
    if (!title || !title[0])
    {
        fail(ctx, callback, "Error: `title` is required.");
        return;
    }
    const char *summary = cJSON_GetStringValue(cJSON_GetObjectItem(args, "summary"));
    const char *detail = cJSON_GetStringValue(cJSON_GetObjectItem(args, "detail"));

    /* Search for related entries BEFORE creating, so the nudge cannot name the new entry
     * itself. Non-blocking: the write still happens; the model decides what to do. */
    sds related = memo_related_hint(m, title, summary);

    sds id = edc_session_memory_create_entry(m, title, summary, detail, ctx->task ? ctx->task->task_id : NULL);
    if (!id)
    {
        if (related)
            sdsfree(related);
        fail(ctx, callback, "Error: could not create the memory entry.");
        return;
    }
    struct edc_memo_entry *e = edc_session_memory_entry(m, id);
    uint64_t seq = edc_session_memory_add_event(m, id, EDC_MEMO_CREATED, title, summary);
    if (e)
    {
        e->created_seq = seq;
        e->updated_seq = seq;
        const char *src = memo_sources_of(ctx);
        if (src)
            e->sources = strdup(src);
    }

    sds out = sdscatprintf(sdsempty(), "Stored as %s (event #%llu). Other tasks see it on their next request.", id,
                           (unsigned long long)seq);
    if (related)
        out = sdscatprintf(out,
                           "\n\nNote: existing entries look related:\n%s\n"
                           "If this is a duplicate, merge into the existing entry with memory_update and retire "
                           "this new one with memory_supersede (%s).",
                           related, id);
    sdsfree(related);
    edc_toolbox_emit_tool_message_plain(ctx, "memory_write", out);
    sdsfree(id);
    callback(ctx, out, NULL);
}

/* ── memory_update (merge into an existing entry) ──────────── */

static void memory_update_exec(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                               void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    sds err = NULL;
    struct edc_session_memory *m = memory_of(ctx, &err);
    if (!m)
    {
        callback(ctx, err, edc_error_new(EDC_ERR_INVAL, "%s", err));
        return;
    }

    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(args, "id"));
    if (!id || !id[0])
    {
        fail(ctx, callback, "Error: `id` is required (get ids from memory_search / memory_list).");
        return;
    }
    /* Empty strings are "not given": only a non-empty field overwrites. */
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(args, "title"));
    const char *summary = cJSON_GetStringValue(cJSON_GetObjectItem(args, "summary"));
    const char *detail = cJSON_GetStringValue(cJSON_GetObjectItem(args, "detail"));
    if (title && !title[0])
        title = NULL;
    if (summary && !summary[0])
        summary = NULL;
    if (detail && !detail[0])
        detail = NULL;
    if (!title && !summary && !detail)
    {
        fail(ctx, callback, "Error: nothing to update; pass at least one of title / summary / detail.");
        return;
    }

    uint64_t expected_rev = 0;
    if (!json_u64(args, "expected_rev", &expected_rev))
    {
        fail(ctx, callback,
             "Error: `expected_rev` is required — the entry's rev when you read it (from memory_get). It blocks "
             "overwriting a newer edit.");
        return;
    }

    uint64_t now_rev = 0;
    enum edc_memo_write_result r = edc_session_memory_update(m, id, title, summary, detail, expected_rev, &now_rev);
    if (r == EDC_MEMO_WRITE_NOT_FOUND)
    {
        fail(ctx, callback, "Error: no memory entry with id \"%s\" (ids are stable; list them with memory_list).", id);
        return;
    }
    if (r == EDC_MEMO_WRITE_CONFLICT)
    {
        sds msg = memo_conflict_msg(m, id, expected_rev, now_rev);
        callback(ctx, msg, edc_error_new(EDC_ERR_INVAL, "%s", msg));
        return;
    }

    sds out = sdscatprintf(sdsempty(), "Updated %s (rev %llu). Other tasks see it on their next request.", id,
                           (unsigned long long)now_rev);
    edc_toolbox_emit_tool_message_plain(ctx, "memory_update", out);
    callback(ctx, out, NULL);
}

/* ── memory_supersede (retire an entry) ────────────────────── */

static void memory_supersede_exec(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                  void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    sds err = NULL;
    struct edc_session_memory *m = memory_of(ctx, &err);
    if (!m)
    {
        callback(ctx, err, edc_error_new(EDC_ERR_INVAL, "%s", err));
        return;
    }

    cJSON *arr = cJSON_GetObjectItem(args, "ids");
    int n = (arr && cJSON_IsArray(arr)) ? cJSON_GetArraySize(arr) : 0;
    if (n <= 0)
    {
        fail(ctx, callback, "Error: `ids` must be a non-empty array of entry ids, e.g. [\"m9\",\"m12\"].");
        return;
    }

    const char **ids = calloc((size_t)n, sizeof(*ids));
    if (!ids)
    {
        fail(ctx, callback, "Error: out of memory.");
        return;
    }
    for (int i = 0; i < n; i++)
    {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        ids[i] = (it && cJSON_IsString(it)) ? it->valuestring : NULL;
    }

    if (!edc_session_memory_supersede(m, ids, (size_t)n))
    {
        /* Atomic refusal (all-or-nothing): name the ids that do not resolve. */
        sds unknown = sdsempty();
        for (int i = 0; i < n; i++)
        {
            if (!ids[i] || !edc_session_memory_entry(m, ids[i]))
                unknown = sdscatprintf(unknown, "%s%s", sdslen(unknown) ? ", " : "",
                                       ids[i] ? ids[i] : "(not a string)");
        }
        fail(ctx, callback, "Error: these ids are unknown — nothing retired (atomic): %s.", unknown);
        sdsfree(unknown);
        free(ids);
        return;
    }

    sds list = sdsempty();
    for (int i = 0; i < n; i++)
        list = sdscatprintf(list, "%s%s", i ? ", " : "", ids[i]);
    sds out = sdscatprintf(sdsempty(), "Retired %d entr%s: %s. Kept (never deleted) and shown as (retired).", n,
                           n == 1 ? "y" : "ies", list);
    sdsfree(list);
    free(ids);
    edc_toolbox_emit_tool_message_plain(ctx, "memory_supersede", out);
    callback(ctx, out, NULL);
}

/* ── Registration ──────────────────────────────────────────── */

void edc_tools_memory_register(struct edc_toolbox *tb)
{
    const char *scope_note =
        "The memory is scoped to the *session* (shared by every task and group in it). It holds durable knowledge: "
        "decisions, constraints, gotchas, external facts — never progress logs, todos or transient conclusions.";

    {
        const char *desc = "Search this session's shared memory entries by keyword (matches title, summary and "
                           "detail). Returns one index line per hit; use memory_get for the full detail.";
        struct edc_tool *tool = edc_tool_new("memory_search", desc,
                                             edc_tool_param_new("query", edc_param_str_type("Keyword to look for", NULL), true),
                                             memory_search_exec);
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "List this session's shared memory entries as index lines, newest ids included. Page "
                           "through with offset/n (n is capped at 50).";
        struct edc_tool_param *params = edc_tool_param_new("offset", edc_param_int_type("Start index (0 = first)", 0, 100000), false);
        params->next = edc_tool_param_new("n", edc_param_int_type("How many to list (default 20, max 50)", 1, 50), false);
        const char *d2 = "List this session's shared memory entries (index lines: id, title, summary).";
        struct edc_tool *tool = edc_tool_new("memory_list", d2, params, memory_list_exec);
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "Fetch the full detail of one session-memory entry by id (ids come from memory_list or "
                           "memory_search / the session memory notice).";
        struct edc_tool *tool = edc_tool_new("memory_get", desc,
                                             edc_tool_param_new("id", edc_param_str_type("Entry id, e.g. \"m12\"", NULL), true),
                                             memory_get_exec);
        edc_toolbox_register(tb, tool);
    }
    {
        sds desc = sdscatprintf(sdsempty(),
                                "Create a NEW entry in this session's shared memory. %s Keep title short, put the "
                                "reasoning / error text in detail; never a progress log. To change or merge into an "
                                "existing entry use memory_update; to retire one use memory_supersede.",
                                scope_note);
        struct edc_tool_param *params = edc_tool_param_new("title", edc_param_str_type("One-line title", NULL), true);
        params->next = edc_tool_param_new("summary", edc_param_str_type("One or two sentences", NULL), false);
        params->next->next = edc_tool_param_new("detail", edc_param_str_type("Full detail: paths, commands, error text", NULL), false);
        struct edc_tool *tool = edc_tool_new("memory_write", desc, params, memory_write_exec);
        sdsfree(desc);
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "Update an existing session-memory entry IN PLACE: merge new or corrected content into "
                           "it. Pass the `id` (from memory_search / memory_list) plus any of title / summary / "
                           "detail to overwrite; omitted fields keep their old value. `expected_rev` is the rev you "
                           "read (from memory_get) and is required: if the entry changed since then the update is "
                           "refused, so a stale snapshot cannot clobber a newer edit.";
        struct edc_tool_param *params = edc_tool_param_new("id", edc_param_str_type("Entry id, e.g. \"m12\"", NULL), true);
        params->next =
            edc_tool_param_new("expected_rev", edc_param_int_type("The entry's rev from memory_get (required)", 1,
                                                                  2000000000),
                               true);
        params->next->next = edc_tool_param_new("title", edc_param_str_type("New title (omit to keep)", NULL), false);
        params->next->next->next = edc_tool_param_new("summary", edc_param_str_type("New summary (omit to keep)", NULL), false);
        params->next->next->next->next =
            edc_tool_param_new("detail", edc_param_str_type("New detail (omit to keep)", NULL), false);
        struct edc_tool *tool = edc_tool_new("memory_update", desc, params, memory_update_exec);
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "Mark one or more session-memory entries as retired (obsolete or wrong). Entries are kept "
                           "(never deleted) and shown as (retired). ATOMIC: if any id is unknown, none is retired. "
                           "Ids come from memory_search / memory_list / memory_get.";
        struct edc_tool_param *sp = edc_tool_param_new(
            "ids",
            edc_param_array_type("Entry ids to retire, e.g. [\"m9\",\"m12\"]",
                                 edc_param_str_type("Entry id, e.g. \"m9\"", NULL)),
            true);
        struct edc_tool *tool = edc_tool_new("memory_supersede", desc, sp, memory_supersede_exec);
        edc_toolbox_register(tb, tool);
    }
}
