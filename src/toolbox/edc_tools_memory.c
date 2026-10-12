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
    callback(ctx, detail, NULL);
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

    sds id = edc_session_memory_create_entry(m, title, summary, detail, ctx->task ? ctx->task->task_id : NULL);
    if (!id)
    {
        fail(ctx, callback, "Error: could not create the memory entry.");
        return;
    }
    struct edc_memo_entry *e = edc_session_memory_entry(m, id);
    uint64_t seq = edc_session_memory_add_event(m, id, EDC_MEMO_CREATED, title, summary);
    if (e)
    {
        e->created_seq = seq;
        e->updated_seq = seq;
        if (ctx->task && ctx->task->group)
            e->sources = strdup(ctx->task->group->group_id ? ctx->task->group->group_id : "");
    }

    sds out = sdscatprintf(sdsempty(), "Stored as %s (event #%llu). Other tasks see it on their next request.", id,
                           (unsigned long long)seq);
    edc_toolbox_emit_tool_message_plain(ctx, "memory_write", out);
    sdsfree(id);
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
                                "Record durable knowledge in this session's shared memory. %s Keep title short, put "
                                "the reasoning/error text in detail. Use it when something will matter to other "
                                "tasks later; do not use it as a progress log.",
                                scope_note);
        struct edc_tool_param *params = edc_tool_param_new("title", edc_param_str_type("One-line title", NULL), true);
        params->next = edc_tool_param_new("summary", edc_param_str_type("One or two sentences", NULL), false);
        params->next->next = edc_tool_param_new("detail", edc_param_str_type("Full detail: paths, commands, error text", NULL), false);
        struct edc_tool *tool = edc_tool_new("memory_write", desc, params, memory_write_exec);
        sdsfree(desc);
        edc_toolbox_register(tb, tool);
    }
}
