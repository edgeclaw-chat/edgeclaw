#include "edc_tools_memory.h"

#include "edc_compat.h"
#include "edc_datetime_utils.h"
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

/* ── echo header = the CALL, not just the tool name ──────────
 * The TOOL echo's header is the line the user reads to know WHICH question was asked;
 * the result alone does not say it (index rows / "Stored as m1" never name what was
 * searched, fetched or written). Same convention as the shell tools, whose header is
 * the command itself. Pure function over the request's args (never touches the entry
 * table), so it is unit-testable on its own. */

/* Append a bracketed list: the string items of `arr`, or the `id` of its object items
 * (memory_update's entries). Missing/ill-typed items degrade to "?" -- the header is
 * display-only and must never fail a call. */
static sds memo_header_add_list(sds h, cJSON *arr, bool objects)
{
    h = sdscat(h, "[");
    int n = (arr && cJSON_IsArray(arr)) ? cJSON_GetArraySize(arr) : 0;
    for (int i = 0; i < n; i++)
    {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        const char *s = NULL;
        if (objects)
            s = cJSON_IsObject(it) ? cJSON_GetStringValue(cJSON_GetObjectItem(it, "id")) : NULL;
        else
            s = cJSON_IsString(it) ? it->valuestring : NULL;
        h = sdscatprintf(h, "%s%s", i ? " " : "", (s && *s) ? s : "?");
    }
    return sdscat(h, "]");
}

sds edc_tools_memory_call_header(const char *name, cJSON *args)
{
    sds h = sdsnew(name ? name : "memory");
    if (!args)
        return h;

    if (strcmp(h, "memory_search") == 0)
    {
        h = sdscat(h, " patterns=");
        h = memo_header_add_list(h, cJSON_GetObjectItem(args, "patterns"), false);
    }
    else if (strcmp(h, "memory_get") == 0 || strcmp(h, "memory_supersede") == 0)
    {
        h = sdscat(h, " ids=");
        h = memo_header_add_list(h, cJSON_GetObjectItem(args, "ids"), false);
    }
    else if (strcmp(h, "memory_list") == 0)
    {
        /* the requested window (the page is also in the result's first line) */
        h = sdscatprintf(h, " offset=%d n=%d", json_int(args, "offset", 0), json_int(args, "n", 20));
    }
    else if (strcmp(h, "memory_write") == 0)
    {
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(args, "title"));
        h = sdscatprintf(h, " title=%s", (title && *title) ? title : "?");
    }
    else if (strcmp(h, "memory_update") == 0)
    {
        h = sdscat(h, " ids=");
        h = memo_header_add_list(h, cJSON_GetObjectItem(args, "entries"), true);
    }
    return h;
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

    cJSON *arr = cJSON_GetObjectItem(args, "patterns");
    int n = (arr && cJSON_IsArray(arr)) ? cJSON_GetArraySize(arr) : 0;
    if (n <= 0)
    {
        fail(ctx, callback, "Error: `patterns` must be a non-empty array of POSIX ERE (grep -E) regexes.");
        return;
    }

    sds out = sdsempty();
    for (int i = 0; i < n; i++)
    {
        const char *pat = cJSON_GetStringValue(cJSON_GetArrayItem(arr, i));
        if (i)
            out = sdscat(out, "\n\n");
        if (!pat || !pat[0])
        {
            out = sdscat(out, "(empty pattern)");
            continue;
        }
        size_t hits = 0;
        int bad = 0;
        sds lines = edc_session_memory_search_regex_lines(m, pat, 20, &hits, &bad);
        if (bad)
            out = sdscatprintf(out, "__MEMORY_SEARCH__ invalid regex: %s", pat);
        else
            out = sdscatprintf(out, "__MEMORY_SEARCH__ %zu hit(s) for /%s/\n%s", hits, pat,
                               sdslen(lines) ? lines : "(no match)");
        sdsfree(lines);
    }
    sds header = edc_tools_memory_call_header("memory_search", args);
    edc_toolbox_emit_tool_message_plain(ctx, header, out);
    sdsfree(header);
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
    sds header = edc_tools_memory_call_header("memory_list", args);
    edc_toolbox_emit_tool_message_plain(ctx, header, out);
    sdsfree(header);
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

    cJSON *arr = cJSON_GetObjectItem(args, "ids");
    int n = (arr && cJSON_IsArray(arr)) ? cJSON_GetArraySize(arr) : 0;
    if (n <= 0)
    {
        fail(ctx, callback, "Error: `ids` must be a non-empty array of entry ids, e.g. [\"m1\",\"m12\"].");
        return;
    }

    sds out = sdsempty();
    for (int i = 0; i < n; i++)
    {
        const char *id = cJSON_GetStringValue(cJSON_GetArrayItem(arr, i));
        if (i)
            out = sdscat(out, "\n\n");
        if (!id || !id[0])
        {
            out = sdscat(out, "- (invalid id)");
            continue;
        }
        sds detail = edc_session_memory_get_detail(m, id);
        struct edc_memo_entry *e = edc_session_memory_entry(m, id);
        if (!detail)
        {
            out = sdscatprintf(out, "- [%s] (no such entry; list ids with memory_list)", id);
            continue;
        }
        /* Surface the entry's rev: memory_update needs it as expected_rev, so a write
         * built on this read cannot clobber a newer edit. */
        out = sdscatprintf(out, "- [%s] rev %llu\n%s", id, (unsigned long long)(e ? e->updated_seq : 0), detail);
        sdsfree(detail);
    }
    sds header = edc_tools_memory_call_header("memory_get", args);
    edc_toolbox_emit_tool_message_plain(ctx, header, out);
    sdsfree(header);
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

    sds when = edc_datetime_format_min((time_t)(e ? e->created_at : 0));
    sds out = sdscatprintf(sdsempty(), "Stored as %s (rev %llu, %s). Other tasks see it on their next request.", id,
                           (unsigned long long)seq, when);
    sdsfree(when);
    if (related)
        out = sdscatprintf(out,
                           "\n\nNote: existing entries look related:\n%s\n"
                           "If this is a duplicate, merge into the existing entry with memory_update and retire "
                           "this new one with memory_supersede (%s).",
                           related, id);
    sdsfree(related);
    sds header = edc_tools_memory_call_header("memory_write", args);
    edc_toolbox_emit_tool_message_plain(ctx, header, out);
    sdsfree(header);
    sdsfree(id);
    callback(ctx, out, NULL);
}

/* ── memory_update (merge into an existing entry) ──────────── */

/* ── memory_update (update one or more entries; atomic) ────── */

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

    cJSON *arr = cJSON_GetObjectItem(args, "entries");
    int n = (arr && cJSON_IsArray(arr)) ? cJSON_GetArraySize(arr) : 0;
    if (n <= 0)
    {
        fail(ctx, callback, "Error: `entries` must be a non-empty array of update objects.");
        return;
    }

    const char **ids = calloc((size_t)n, sizeof(*ids));
    const char **titles = calloc((size_t)n, sizeof(*titles));
    const char **summaries = calloc((size_t)n, sizeof(*summaries));
    const char **details = calloc((size_t)n, sizeof(*details));
    uint64_t *revs = calloc((size_t)n, sizeof(*revs));
    if (!ids || !titles || !summaries || !details || !revs)
    {
        free(ids);
        free(titles);
        free(summaries);
        free(details);
        free(revs);
        fail(ctx, callback, "Error: out of memory.");
        return;
    }

    /* Pass 1: validate the whole batch; any bad item refuses the whole update. */
    sds bad = sdsempty();
    bool ok = true;
    for (int i = 0; i < n; i++)
    {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(it))
        {
            bad = sdscatprintf(bad, "%s#%d: not an object", sdslen(bad) ? "; " : "", i);
            ok = false;
            continue;
        }
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
        /* Empty strings are "not given": only a non-empty field overwrites. */
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(it, "title"));
        const char *summary = cJSON_GetStringValue(cJSON_GetObjectItem(it, "summary"));
        const char *detail = cJSON_GetStringValue(cJSON_GetObjectItem(it, "detail"));
        if (title && !title[0])
            title = NULL;
        if (summary && !summary[0])
            summary = NULL;
        if (detail && !detail[0])
            detail = NULL;
        uint64_t rev = 0;
        bool has_rev = json_u64(it, "expected_rev", &rev);

        bool dup = false;
        for (int j = 0; j < i; j++)
            if (ids[j] && id && strcmp(ids[j], id) == 0)
                dup = true;

        struct edc_memo_entry *e = (id && id[0]) ? edc_session_memory_entry(m, id) : NULL;
        const char *pfx = sdslen(bad) ? "; " : "";
        if (!id || !id[0])
            bad = sdscatprintf(bad, "%s#%d: missing id", pfx, i), ok = false;
        else if (!e)
            bad = sdscatprintf(bad, "%s%s: unknown id", pfx, id), ok = false;
        else if (!has_rev)
            bad = sdscatprintf(bad, "%s%s: missing expected_rev", pfx, id), ok = false;
        else if (rev != e->updated_seq)
            bad = sdscatprintf(bad, "%s%s: stale expected_rev (now %llu)", pfx, id, (unsigned long long)e->updated_seq),
            ok = false;
        else if (dup)
            bad = sdscatprintf(bad, "%s%s: duplicate id in batch", pfx, id), ok = false;
        else if (!title && !summary && !detail)
            bad = sdscatprintf(bad, "%s%s: no field to update", pfx, id), ok = false;
        else
        {
            ids[i] = id;
            titles[i] = title;
            summaries[i] = summary;
            details[i] = detail;
            revs[i] = rev;
        }
    }
    if (!ok)
    {
        fail(ctx, callback, "Error: invalid update batch — nothing changed (atomic): %s.", bad);
        sdsfree(bad);
        free(ids);
        free(titles);
        free(summaries);
        free(details);
        free(revs);
        return;
    }
    sdsfree(bad);

    /* Pass 2: apply. Single-threaded, so the revs validated above still hold. */
    sds list = sdsempty();
    for (int i = 0; i < n; i++)
    {
        uint64_t now = 0;
        edc_session_memory_update(m, ids[i], titles[i], summaries[i], details[i], revs[i], &now);
        list = sdscatprintf(list, "%s%s", i ? ", " : "", ids[i]);
    }
    struct edc_memo_entry *e0 = n > 0 ? edc_session_memory_entry(m, ids[0]) : NULL;
    sds when = edc_datetime_format_min((time_t)(e0 ? e0->updated_at : 0));
    sds out = sdscatprintf(sdsempty(), "Updated %d entr%s: %s (%s). Other tasks see it on their next request.", n,
                           n == 1 ? "y" : "ies", list, when);
    sdsfree(when);
    sdsfree(list);
    free(ids);
    free(titles);
    free(summaries);
    free(details);
    free(revs);
    sds header = edc_tools_memory_call_header("memory_update", args);
    edc_toolbox_emit_tool_message_plain(ctx, header, out);
    sdsfree(header);
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
    sds header = edc_tools_memory_call_header("memory_supersede", args);
    edc_toolbox_emit_tool_message_plain(ctx, header, out);
    sdsfree(header);
    callback(ctx, out, NULL);
}

/* ── memory_delete (an entry leaves the views; the row stays in the data) ── */

static void memory_delete_exec(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
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

    if (!edc_session_memory_delete(m, ids, (size_t)n))
    {
        /* Atomic refusal (all-or-nothing): name the ids that do not resolve. */
        sds unknown = sdsempty();
        for (int i = 0; i < n; i++)
        {
            if (!ids[i] || !edc_session_memory_entry(m, ids[i]))
                unknown = sdscatprintf(unknown, "%s%s", sdslen(unknown) ? ", " : "",
                                       ids[i] ? ids[i] : "(not a string)");
        }
        fail(ctx, callback, "Error: these ids are unknown — nothing deleted (atomic): %s.", unknown);
        sdsfree(unknown);
        free(ids);
        return;
    }

    sds list = sdsempty();
    for (int i = 0; i < n; i++)
        list = sdscatprintf(list, "%s%s", i ? ", " : "", ids[i]);
    sds out = sdscatprintf(sdsempty(),
                           "Deleted %d entr%s for real: %s. They are gone from the memory (no longer listable, "
                           "searchable, or fetchable by id).",
                           n, n == 1 ? "y" : "ies", list);
    sdsfree(list);
    free(ids);
    sds header = edc_tools_memory_call_header("memory_delete", args);
    edc_toolbox_emit_tool_message_plain(ctx, header, out);
    sdsfree(header);
    callback(ctx, out, NULL);
}

/* ── Registration ──────────────────────────────────────────── */

void edc_tools_memory_register(struct edc_toolbox *tb)
{
    const char *scope_note =
        "The memory is scoped to the *session* (shared by every task and group in it). It holds durable knowledge: "
        "decisions, constraints, gotchas, external facts — never progress logs, todos or transient conclusions.";

    {
        const char *desc = "Search this session's shared memory entries with one or more POSIX ERE regexes "
                           "(grep -E syntax), case-insensitive, matched anywhere in title / summary / detail. "
                           "Returns one index line per hit, grouped per pattern; use memory_get for the full detail.";
        struct edc_tool *tool = edc_tool_new(
            "memory_search", desc,
            edc_tool_param_new("patterns",
                               edc_param_array_type("Regexes to look for, e.g. [\"bazel\",\"^m3\\\\b\"]",
                                                    edc_param_str_type("POSIX ERE (grep -E)", NULL)),
                               true),
            memory_search_exec);
        edc_tool_set_keep_full_output(tool, true); /* the agent's own store: never cut silently */
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "List this session's shared memory entries as index lines, newest ids included. Page "
                           "through with offset/n (n is capped at 50).";
        struct edc_tool_param *params = edc_tool_param_new("offset", edc_param_int_type("Start index (0 = first)", 0, 100000), false);
        params->next = edc_tool_param_new("n", edc_param_int_type("How many to list (default 20, max 50)", 1, 50), false);
        const char *d2 = "List this session's shared memory entries (index lines: id, title, summary).";
        struct edc_tool *tool = edc_tool_new("memory_list", d2, params, memory_list_exec);
        edc_tool_set_keep_full_output(tool, true); /* the agent's own store: never cut silently */
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "Fetch the full detail (and rev) of one or more session-memory entries by id. Ids come "
                           "from memory_list / memory_search / the session memory notice.";
        struct edc_tool *tool = edc_tool_new(
            "memory_get", desc,
            edc_tool_param_new("ids",
                               edc_param_array_type("Entry ids, e.g. [\"m1\",\"m12\"]",
                                                    edc_param_str_type("Entry id, e.g. \"m12\"", NULL)),
                               true),
            memory_get_exec);
        edc_tool_set_keep_full_output(tool, true); /* the agent's own store: never cut silently */
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
        edc_tool_set_keep_full_output(tool, true); /* the agent's own store: never cut silently */
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "Update one or more session-memory entries IN PLACE (merge new / corrected content). Each "
                           "entry needs its `id` and the `expected_rev` you read (from memory_get) plus any of "
                           "title / summary / detail to overwrite; omitted fields keep their old value. ATOMIC: if "
                           "any item is unknown or stale, none is updated.";
        struct edc_param_object_property props[] = {
            {"id", edc_param_str_type("Entry id, e.g. \"m12\"", NULL), true},
            {"expected_rev", edc_param_int_type("The entry's rev from memory_get", 1, 2000000000), true},
            {"title", edc_param_str_type("New title (omit to keep)", NULL), false},
            {"summary", edc_param_str_type("New summary (omit to keep)", NULL), false},
            {"detail", edc_param_str_type("New detail (omit to keep)", NULL), false},
        };
        struct edc_tool *tool = edc_tool_new(
            "memory_update", desc,
            edc_tool_param_new(
                "entries",
                edc_param_array_type("Updates, e.g. [{\"id\":\"m12\",\"expected_rev\":47,\"summary\":\"...\"}]",
                                     edc_param_object_type("One entry update", props, G_N_ELEMENTS(props))),
                true),
            memory_update_exec);
        edc_tool_set_keep_full_output(tool, true); /* the agent's own store: never cut silently */
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "Mark one or more session-memory entries as retired (obsolete or wrong). Retired entries "
                           "are kept and stay visible as (retired); use memory_delete when a fact should leave the "
                           "views entirely. ATOMIC: if any id is unknown, none is retired. Ids come from "
                           "memory_search / memory_list / memory_get.";
        struct edc_tool_param *sp = edc_tool_param_new(
            "ids",
            edc_param_array_type("Entry ids to retire, e.g. [\"m9\",\"m12\"]",
                                 edc_param_str_type("Entry id, e.g. \"m9\"", NULL)),
            true);
        struct edc_tool *tool = edc_tool_new("memory_supersede", desc, sp, memory_supersede_exec);
        edc_tool_set_keep_full_output(tool, true); /* the agent's own store: never cut silently */
        edc_toolbox_register(tb, tool);
    }
    {
        const char *desc = "Delete one or more session-memory entries for real (use this for a fact that is simply "
                           "wrong or no longer wanted, rather than cluttering the index). Unlike memory_supersede "
                           "(which only marks an entry obsolete and keeps it visible), a deleted entry is removed: "
                           "it can no longer be listed, searched or fetched. ATOMIC: if any id is unknown, none is "
                           "deleted. Ids come from memory_search / memory_list / memory_get.";
        struct edc_tool_param *dp = edc_tool_param_new(
            "ids",
            edc_param_array_type("Entry ids to delete, e.g. [\"m9\",\"m12\"]",
                                 edc_param_str_type("Entry id, e.g. \"m9\"", NULL)),
            true);
        struct edc_tool *tool = edc_tool_new("memory_delete", desc, dp, memory_delete_exec);
        edc_tool_set_keep_full_output(tool, true); /* the agent's own store: never cut silently */
        edc_toolbox_register(tb, tool);
    }
}
