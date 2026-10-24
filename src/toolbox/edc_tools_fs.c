#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_platform.h"
#include "edc_sandbox.h"
#include "edc_scheduler.h"
#include "edc_string_utils.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "sds.h"
#include "utarray.h"
#include <edc_param_types.h>
#include <string.h>

static UT_icd ut_size_t_icd = {sizeof(size_t), NULL, NULL, NULL};
static UT_icd ut_int64_icd = {sizeof(int64_t), NULL, NULL, NULL};
static UT_icd ut_uint_icd = {sizeof(unsigned int), NULL, NULL, NULL};

/* ── The async shell around fs I/O ───────────────────────────
 *
 * Why: file I/O blocks, and tools run ON THE RUNNER THREAD -- a slow I/O (large file /
 * cold cache / NFS) would freeze ALL actors, not just the task that issued it, violating
 * the rule "the runner thread never blocks".
 *
 * The split:
 *   runner: sandbox check (reads conf's ro/rw/deny tables) -> a verified absolute path
 *   pool:   pure file I/O (uv_fs_*(..., NULL)'s synchronous path does NOT use the thread
 *           pool -- it makes the syscall on the caller thread -- so calling it on a pool
 *           thread makes the I/O happen there)
 *   runner: after_cb -> send the tool message + call callback
 *
 * The sandbox check MUST stay on the runner: /sandbox add-* also mutates those three
 * tables on the runner thread, so moving "check + I/O" wholesale into the pool would
 * create a new data race.
 *
 * Two lifetime rules:
 *   1. args must NOT enter the pool. Once exec returns, the toolbox frees the req (and
 *      args) while an async tool's exec returns immediately -- a pool thread reading args
 *      is a use-after-free. Everything the pool needs is copied into the job on the
 *      runner.
 *   2. ctx is safe by contrast. call_ctx is managed by the toolbox's inflight registry
 *      and is freed only in the callback -- holding it across this hop is architecture
 *      the code already supports (the "TODO: async exec" note at the end of
 *      edc_toolbox_actor.c was its placeholder; the shell already does this).
 *
 * Aside (no behaviour change): the pool defaults to 4. Four stuck fs ops make later ones
 * queue, but the runner stays alive -- exactly the goal of this change. No deadline for
 * fs: a pool thread cannot be interrupted. */
struct fs_job
{
    uv_work_t req;
    struct edc_toolbox_call_ctx *ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *);
    sds (*io)(struct fs_job *, EdcError **); /* runs on a pool thread */
    sds safe_path;                           /* computed on the runner; read-only in the pool */
    sds arg_path;                            /* the raw path, for the reply text only */
    sds header;                              /* send a tool message only when non-NULL (edit_file does not, as before) */
    sds content;                             /* write_file's body */
    sds old_text;                            /* edit_file */
    sds new_text;
    /* read_file's line window (1-based offset; 0 limit = the tool's own ceiling). Set on the
     * runner, read on the pool. */
    int64_t offset;
    int64_t limit;
    bool replace_all;
    int64_t occurrence;
    int64_t line_hint;
    int64_t expected_replacements;
    sds result; /* filled by the pool thread */
    EdcError *error;
};

static void edc_fs_work_cb(uv_work_t *req)
{
    struct fs_job *job = (struct fs_job *)req->data;
    job->result = job->io(job, &job->error);
}

static void edc_fs_after_cb(uv_work_t *req, int status)
{
    (void)status;
    struct fs_job *job = (struct fs_job *)req->data;

    if (job->result && job->header)
        edc_toolbox_emit_tool_message_plain(job->ctx, job->header, job->result);
    job->callback(job->ctx, job->result, job->error);

    if (job->safe_path)
        sdsfree(job->safe_path);
    if (job->arg_path)
        sdsfree(job->arg_path);
    if (job->header)
        sdsfree(job->header);
    if (job->content)
        sdsfree(job->content);
    if (job->old_text)
        sdsfree(job->old_text);
    if (job->new_text)
        sdsfree(job->new_text);
    free(job);
}

/* Runner half: build the job. tool_name NULL -> no tool message (edit_file never sends one). */
static struct fs_job *edc_fs_job_new(struct edc_toolbox_call_ctx *ctx,
                                     void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *),
                                     sds (*io)(struct fs_job *, EdcError **), const char *tool_name,
                                     const char *arg_path, sds safe_path)
{
    struct fs_job *job = calloc(1, sizeof(*job));
    job->req.data = job;
    job->ctx = ctx;
    job->callback = callback;
    job->io = io;
    job->safe_path = safe_path;
    job->arg_path = sdsnew(arg_path ? arg_path : "");
    if (tool_name)
        job->header = sdscatprintf(sdsempty(), "%s %s", tool_name, arg_path ? arg_path : "");
    return job;
}

/* Runner half: sandbox check -> enqueue. On failure reply on the runner directly (same path as the old synchronous failure). */
static void edc_fs_kick_path_tool(struct edc_toolbox_call_ctx *ctx, cJSON *args, const char *tool_name, bool write,
                                  void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *),
                                  sds (*io)(struct fs_job *, EdcError **),
                                  void (*fill)(struct fs_job *, cJSON *))
{
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    EdcError *error = NULL;
    sds safe = edc_sandbox_verify_path_at(ctx->tb->conf, edc_scheduler_task_workdir(ctx->task),
                                          path, write, &error);
    if (!safe)
    {
        callback(ctx, NULL, error);
        return;
    }
    struct fs_job *job = edc_fs_job_new(ctx, callback, io, tool_name, path, safe);
    /* |fill| runs on the runner, before the job is handed to the pool: this is where a tool
     * copies the rest of its arguments (the pool must never touch `args`, §4.4). */
    if (fill)
        fill(job, args);
    uv_queue_work(ctx->tb->scheduler->loop, &job->req, edc_fs_work_cb, edc_fs_after_cb);
}

/*
 * Below is the I/O half of the four tools: it only consumes the job, touching NEITHER
 * conf NOR args.
 */

struct edc_list_dir_ctx
{
    sds result;
};

static void edc_tool_list_dir_cb(const char *name, int type, void *userdata)
{
    struct edc_list_dir_ctx *lctx = (struct edc_list_dir_ctx *)userdata;
    const char *prefix = (type == UV_DIRENT_DIR) ? "[DIR]  " : "[FILE] ";
    lctx->result = sdscatprintf(lctx->result, "%s%s\n", prefix, name);
}

/* The largest file a tool will read AT ALL. Larger than what read_file returns (64 KiB) on
 * purpose: paging through a big file must work, so the ceiling is on "one read", not on "the
 * file". Past this, the answer is "use offset/limit" or "this file cannot be edited", never an
 * allocation the process may not survive. */
#define EDC_READ_MAX_BYTES_TOTAL ((size_t)8 * 1024 * 1024)

static sds fs_read_for_tool_capped_sized(const char *path, size_t max_bytes, size_t *size_out,
                                        bool *had_bom, EdcError **error);

/* ── A leading UTF-8 BOM: metadata on Windows, content elsewhere ─────────────
 *
 * PowerShell writes UTF-8 WITH a BOM by default (and so do most Windows editors), so a file the
 * model has just created through exec_shell starts with U+FEFF. Left in, it shows up as an
 * invisible character in read_file's result AND makes edit_file's first-line match fail
 * against a string that looks identical to the one on screen.
 *
 * Stripped here, at the TOOL boundary, and NOT inside edc_file_read(): that reader also feeds
 * config and session state, where the bytes must stay exactly as they are on every platform.
 * Whether a BOM is metadata at all is the platform's answer (edc_platform_bom_is_metadata).
 *
 * |had_bom| reports what was dropped, so a rewrite can put it back: an edit must not change
 * bytes it was not asked to change. */
static sds fs_read_for_tool(const char *path, bool *had_bom, EdcError **error)
{
    /* A tool never reads an unbounded file: the path came from the model, so the size is not
     * something this process gets to trust (a 10 GB file used to be a 10 GB malloc). The
     * ceiling is checked before the read; read_file can still see any file by paging with
     * offset, and edit_file refuses and says why. */
    return fs_read_for_tool_capped_sized(path, EDC_READ_MAX_BYTES_TOTAL, NULL, had_bom, error);
}

/* |size_out| is the file's size as the reader saw it, which is what tells "over the limit"
 * apart from "could not read": an unreadable file has no size worth reporting. */
/* Strip a leading UTF-8 BOM when the platform treats one as metadata, reporting what was
 * dropped so a rewrite can put it back.
 *
 * Pure, and it takes the platform's answer as an argument rather than asking for it, because
 * edc_platform_bom_is_metadata() is a compile-time constant (true only on Windows). Without
 * the parameter the stripping branch is unreachable on a POSIX host, which is how a broken
 * caller -- edit_file reading through the raw reader, so had_bom was always false and the
 * restore branch below was dead -- went unnoticed for as long as it did.
 *
 * |content| is owned by the caller and is either returned unchanged or freed. */
sds edc_fs_strip_bom_for_tool(sds content, bool bom_is_metadata, bool *had_bom)
{
    if (had_bom)
        *had_bom = false;
    if (!content || !bom_is_metadata)
        return content;
    if (sdslen(content) < 3 || memcmp(content, "\xEF\xBB\xBF", 3) != 0)
        return content;

    if (had_bom)
        *had_bom = true;
    return edc_str_strip_utf8_bom(content);
}

static sds fs_read_for_tool_capped_sized(const char *path, size_t max_bytes, size_t *size_out,
                                        bool *had_bom, EdcError **error)
{
    if (size_out)
        *size_out = 0;

    size_t size = 0;
    sds content = edc_file_read_capped(path, max_bytes, &size, error);
    if (size_out)
        *size_out = size;
    return edc_fs_strip_bom_for_tool(content, edc_platform_bom_is_metadata(), had_bom);
}

/* read_file's own ceilings. They exist because the tool has to bound itself: the toolbox's
 * generic cap (tool_max_output_size, 10 KB by default) cuts from the head with no idea where
 * the next line starts, so a model that hit it could never ask for the rest. This tool
 * declares keep_full_output (see edc_tool_create_read_file) and applies these instead, with a
 * marker that says which lines were returned and how to continue. */
#define EDC_READ_MAX_LINES ((int64_t)2000)
#define EDC_READ_MAX_BYTES ((size_t)64 * 1024)

/* Count the lines of |content|. A trailing newline does not open an extra empty line. */
static int64_t count_lines(const char *p, size_t len)
{
    if (len == 0)
        return 0;
    int64_t n = 0;
    const char *q = p;
    const char *end = p + len;
    while ((q = memchr(q, '\n', (size_t)(end - q))) != NULL)
    {
        n++;
        q++;
    }
    return (p[len - 1] == '\n') ? n : n + 1;
}

/* Take the requested window out of |content|.
 *
 * Two linear passes (count, then locate) rather than a per-line seek: the first version
 * re-scanned from the start for every line it collected, which is quadratic and would stall
 * the pool thread on a large file.
 *
 * When the window is not the whole file, one line at the HEAD says which lines it holds. The
 * head is deliberate: a result that is long enough to be cut is cut from the front, so a
 * trailer would be exactly the part that disappears (the shell tool's notices sit at the head
 * for the same reason). */
sds edc_fs_read_file_window(const char *p, size_t len, int64_t offset, int64_t limit_in)
{
    const int64_t total = count_lines(p, len);
    const int64_t limit = (limit_in > 0) ? limit_in : EDC_READ_MAX_LINES;
    if (offset < 1)
        offset = 1;

    if (total == 0 || offset > total)
        return sdscatprintf(sdsempty(), "[no line %lld: the file has %lld line(s)]\n", (long long)offset,
                            (long long)total);

    /* One pass to locate [start, stop): from `offset`, take lines while both budgets allow.
     * Always on a line boundary, so the model never gets half a line to guess at. */
    size_t pos = 0, start = 0, stop = 0;
    int64_t lineno = 0, taken = 0;
    while (pos < len)
    {
        size_t nl = pos;
        while (nl < len && p[nl] != '\n')
            nl++;
        const size_t line_end = (nl < len) ? nl + 1 : nl;
        lineno++;

        if (lineno < offset)
        {
            pos = line_end;
            continue;
        }
        if (taken >= limit)
            break;
        if (taken == 0)
            start = pos;
        else if ((line_end - start) > EDC_READ_MAX_BYTES)
            break; /* the byte budget ends the window, on a boundary */
        stop = line_end;
        taken++;
        pos = line_end;
    }

    /* A single line can be longer than the whole budget (a minified bundle, a base64 blob).
     * Then the window cannot end on a boundary: cut it, and say so -- the alternative is
     * handing the model a megabyte it never asked for. */
    size_t body_len = stop - start;
    const bool line_cut = body_len > EDC_READ_MAX_BYTES;
    if (line_cut)
        body_len = EDC_READ_MAX_BYTES;

    const bool whole = (offset == 1) && (taken == total) && (stop == len) && !line_cut;
    sds out = sdsempty();
    if (!whole)
        out = sdscatprintf(out, "[lines %lld-%lld of %lld%s]\n", (long long)offset, (long long)(offset + taken - 1),
                           (long long)total, line_cut ? ", cut at 64 KiB" : "");
    out = sdscatlen(out, p + start, body_len);

    /* How to fetch the rest -- worth saying only when something is left. */
    if (line_cut)
        out = sdscat(out, "\n[that line is longer than 64 KiB and was cut]\n");
    else if (!whole && (offset + taken - 1) < total)
        out = sdscatprintf(out, "\n[more lines follow: pass offset=%lld to continue]\n", (long long)(offset + taken));

    return out;
}

static sds edc_tool_read_file_io(struct fs_job *job, EdcError **error)
{
    EdcError *gerror = NULL;
    size_t size = 0;
    sds content = edc_file_read_capped(job->safe_path, EDC_READ_MAX_BYTES_TOTAL, &size, &gerror);
    if (!content)
    {
        /* Refused for size, not for I/O: say what to do, not just how big it is. A message
         * that stops at "8 MB is too big" leaves the model with no next step. */
        if (size > EDC_READ_MAX_BYTES_TOTAL)
            edc_error_set(error, EDC_ERR_INVAL,
                          "Error: '%s' is %zu bytes, over the %zu-byte single-read limit. This tool cannot "
                          "page a file this large; use exec_shell (e.g. `sed -n '1,200p'`, `head`, `tail`) "
                          "to read a range.",
                          job->arg_path ? job->arg_path : "(file)", size, EDC_READ_MAX_BYTES_TOTAL);
        else
            edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
        edc_error_free(gerror);
        return NULL;
    }
    /* The window is computed from the bytes; |content| stays owned here. */
    sds window = edc_fs_read_file_window(content, sdslen(content), job->offset > 0 ? job->offset : 1, job->limit);
    sdsfree(content);
    return window;
}

static sds edc_tool_write_file_io(struct fs_job *job, EdcError **error)
{
    sds parent = edc_path_dirname(job->safe_path);
    if (!edc_dir_create_with_parents(parent, 0755, error))
    {
        sdsfree(parent);
        return NULL;
    }
    sdsfree(parent);

    if (!edc_file_write(job->safe_path, job->content, -1, error))
        return NULL;

    sds result = sdsnew("");
    result = sdscatprintf(result, "Successfully wrote to file %s", job->arg_path);
    return result;
}

static sds edc_tool_list_dir_io(struct fs_job *job, EdcError **error)
{
    EdcError *gerror = NULL;
    struct edc_list_dir_ctx lctx = {.result = sdsnew("")};
    bool ok = edc_dir_foreach(job->safe_path, edc_tool_list_dir_cb, &lctx, &gerror);
    if (!ok)
    {
        sdsfree(lctx.result);
        edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
        edc_error_free(gerror);
        return NULL;
    }
    return lctx.result;
}


/* Runner half: the two window arguments are copied into the job here, because the pool
 * thread must not read `args` (§4.4). Absent/0 keeps the defaults (offset 1, limit 2000). */
static void read_file_fill(struct fs_job *job, cJSON *args)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "offset");
    if (j && cJSON_IsNumber(j) && j->valuedouble > 0)
        job->offset = (int64_t)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(args, "limit");
    if (j && cJSON_IsNumber(j) && j->valuedouble > 0)
        job->limit = (int64_t)j->valuedouble;
}

static void edc_tool_exec_read_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                          void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    edc_fs_kick_path_tool(ctx, args, "read_file", false, callback, edc_tool_read_file_io, read_file_fill);
}

static void edc_tool_create_read_file(struct edc_toolbox *tb)
{
    const char *name = "read_file";
    const char *desc = "Read the contents of a file at the given path.";
    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("The file path to read", NULL), true));
    LL_APPEND(params, edc_tool_param_new("offset", edc_param_int_type("1-based line to start from (default 1)", 1, G_MAXINT64), false));
    LL_APPEND(params, edc_tool_param_new("limit", edc_param_int_type("How many lines to read (default 2000)", 1, 100000), false));
    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_read_file_async);
    edc_tool_set_snippet(tool, "Read a file (whole, or a line range)");
    /* Both halves of this are now true: the window really exists (offset/limit), and the tool
     * really bounds itself at 2000 lines or 64 KiB -- which is why it keeps its output whole
     * instead of letting the generic cap cut it mid-line with no way to continue. */
    edc_tool_set_guidelines(tool, "A read returns at most 2000 lines or 64 KiB; when a file is longer the "
                                  "first line says which lines you got, and offset fetches the rest.");
    edc_tool_set_keep_full_output(tool, true);
    edc_toolbox_register(tb, tool);
}

static void edc_tool_exec_write_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                           void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    /* content must be copied on the runner: args is freed once exec returns (§4.4). */
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "content"));
    EdcError *error = NULL;
    sds safe = edc_sandbox_verify_path_at(ctx->tb->conf, edc_scheduler_task_workdir(ctx->task),
                                          path, true, &error);
    if (!safe)
    {
        callback(ctx, NULL, error);
        return;
    }
    struct fs_job *job = edc_fs_job_new(ctx, callback, edc_tool_write_file_io, "write_file", path, safe);
    job->content = content ? sdsnew(content) : NULL;
    uv_queue_work(ctx->tb->scheduler->loop, &job->req, edc_fs_work_cb, edc_fs_after_cb);
}

static void edc_tool_create_write_file(struct edc_toolbox *tb)
{
    const char *name = "write_file";
    const char *desc = "Write the contents of a file at the given path.";

    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("The file path to write", NULL), true));
    LL_APPEND(params, edc_tool_param_new("content", edc_param_str_type("The content to write", NULL), true));

    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_write_file_async);
    edc_tool_set_snippet(tool, "Write a file (missing parent directories are created)");
    edc_toolbox_register(tb, tool);
}

static void edc_tool_exec_list_dir_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                         void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    edc_fs_kick_path_tool(ctx, args, "list_dir", false, callback, edc_tool_list_dir_io, NULL);
}

static void edc_tool_create_list_dir(struct edc_toolbox *tb)
{
    const char *name = "list_dir";
    const char *desc = "List the contents of a directory at the given path.";

    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("The directory path to list", NULL), true));
    struct edc_tool *tool = edc_tool_new(name, desc, params, edc_tool_exec_list_dir_async);
    edc_tool_set_snippet(tool, "List one directory level");
    edc_tool_set_guidelines(tool, "One level only -- it does not recurse.");
    edc_toolbox_register(tb, tool);
}

static sds edc_edit_file_try_create(const char *path, sds safe_path, const char *old_text, const char *new_text,
                                    EdcError **error)
{
    if (!edc_file_exists(safe_path))
    {
        if (old_text && old_text[0] == '\0')
        {
            sds parent = edc_path_dirname(safe_path);
            edc_dir_create_with_parents(parent, 0755, NULL);
            sdsfree(parent);
            if (!edc_file_write(safe_path, new_text, -1, error))
            {
                return NULL;
            }
            sds msg = sdscatprintf(sdsempty(), "Successfully created %s", path);
            return msg;
        }
        edc_error_set(error, EDC_ERR_EXEC, "Error: File not found: %s", path);
        return NULL;
    }

    if (old_text && old_text[0] == '\0')
    {
        EdcError *gerror = NULL;
        sds content = fs_read_for_tool(safe_path, NULL, &gerror);
        if (!content)
        {
            if (gerror)
            {
                edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
                edc_error_free(gerror);
            }
            return NULL;
        }
        sdstrim(content, " \t\r\n");
        if (content[0] != '\0')
        {
            sdsfree(content);
            edc_error_set(error, EDC_ERR_EXEC, "Error: Cannot create file — %s already exists and is not empty.", path);
            return NULL;
        }
        sdsfree(content);
        if (!edc_file_write(safe_path, new_text, -1, error))
        {
            return NULL;
        }
        sds msg = sdscatprintf(sdsempty(), "Successfully edited %s", path);
        return msg;
    }

    return NULL;
}

static void edc_edit_file_find_matches(const char *content, const char *old_text, size_t old_len, UT_array **positions,
                                       UT_array **line_nums, unsigned int *out_count)
{
    utarray_new(*positions, &ut_size_t_icd);
    utarray_new(*line_nums, &ut_int64_icd);

    /* Guard: strstr with empty needle matches at every position → infinite loop */
    if (old_len == 0)
    {
        *out_count = 0;
        return;
    }

    const char *scan = content;
    int64_t current_line = 1;
    const char *nl_scan = content;

    while ((scan = strstr(scan, old_text)) != NULL)
    {
        size_t pos = (size_t)(scan - content);
        while (nl_scan < scan)
        {
            if (*nl_scan == '\n')
                current_line++;
            nl_scan++;
        }
        utarray_push_back(*positions, &pos);
        utarray_push_back(*line_nums, &current_line);
        scan += old_len;
    }
    *out_count = utarray_len(*positions);
}

static void edc_edit_file_format_match_list(const char *content, size_t content_len, UT_array *positions,
                                            UT_array *line_nums, size_t old_len, unsigned int count,
                                            unsigned int max_preview, sds *out)
{
    unsigned int preview = MIN(count, max_preview);
    for (unsigned int i = 0; i < preview; i++)
    {
        size_t pos = *(size_t *)utarray_eltptr(positions, (unsigned)i);
        int64_t ln = *(int64_t *)utarray_eltptr(line_nums, (unsigned)i);
        size_t ctx_start = pos > 25 ? pos - 25 : 0;
        size_t ctx_end = pos + old_len + 25;
        if (ctx_end > content_len)
            ctx_end = content_len;

        sds ctx = sdsempty();
        for (size_t ci = ctx_start; ci < ctx_end; ci++)
        {
            char c = content[ci];
            if (c == '\n')
                ctx = sdscat(ctx, "\\n");
            else if (c == '\t')
                ctx = sdscat(ctx, "\\t");
            else
                ctx = sdscatprintf(ctx, "%c", c);
        }
        *out = sdscatprintf(*out, "\n  [%u] line %ld: \"%s\"", i + 1, (long)ln, ctx);
        sdsfree(ctx);
    }
    if (count > max_preview)
        *out = sdscatprintf(*out, "\n  ... and %u more matches", count - max_preview);
}

static bool edc_edit_file_select_all(unsigned int count, UT_array **selected, EdcError **error)
{
    for (unsigned int i = 0; i < count; i++)
    {
        unsigned int idx = i;
        utarray_push_back(*selected, &idx);
    }
    return true;
}

static bool edc_edit_file_select_by_occurrence(unsigned int count, int64_t occurrence, UT_array **selected,
                                               EdcError **error)
{
    if ((uint64_t)occurrence > count)
    {
        edc_error_set(error, EDC_ERR_EXEC, "Error: occurrence %ld out of range; old_text appears %u times.",
                      (long)occurrence, count);
        return false;
    }
    unsigned int idx = (unsigned int)(occurrence - 1);
    utarray_push_back(*selected, &idx);
    return true;
}

static bool edc_edit_file_select_by_line_hint(const char *content, size_t content_len, unsigned int count,
                                              UT_array *line_nums, UT_array *positions, size_t old_len,
                                              int64_t line_hint, UT_array **selected, EdcError **error)
{
    int64_t best_dist = G_MAXINT64;
    unsigned int best_idx = 0;
    bool ambiguous = false;

    for (unsigned int i = 0; i < count; i++)
    {
        int64_t ln = *(int64_t *)utarray_eltptr(line_nums, (unsigned)i);
        int64_t dist = ln > line_hint ? ln - line_hint : line_hint - ln;
        if (dist < best_dist)
        {
            best_dist = dist;
            best_idx = i;
            ambiguous = false;
        }
        else if (dist == best_dist)
            ambiguous = true;
    }

    if (ambiguous)
    {
        sds msg = sdsempty();
        edc_edit_file_format_match_list(content, content_len, positions, line_nums, old_len, count, 5, &msg);
        edc_error_set(error, EDC_ERR_EXEC, "Error: line_hint %ld is ambiguous; old_text appears %u times.%s",
                      (long)line_hint, count, msg);
        sdsfree(msg);
        return false;
    }

    utarray_push_back(*selected, &best_idx);
    return true;
}

static bool edc_edit_file_select_matches(const char *content, size_t content_len, unsigned int count,
                                         UT_array *line_nums, UT_array *positions, size_t old_len, bool replace_all,
                                         int64_t occurrence, int64_t line_hint, UT_array **selected, EdcError **error)
{
    utarray_new(*selected, &ut_uint_icd);

    if ((replace_all ? 1 : 0) + (occurrence > 0 ? 1 : 0) + (line_hint > 0 ? 1 : 0) > 1)
    {
        edc_error_set(error, EDC_ERR_EXEC, "Error: occurrence, line_hint, and replace_all are mutually exclusive.");
        return false;
    }

    if (count == 0)
    {
        edc_error_set(error, EDC_ERR_EXEC, "Error: internal error — no matches to select from.");
        return false;
    }

    if (replace_all)
        return edc_edit_file_select_all(count, selected, error);

    if (occurrence > 0)
        return edc_edit_file_select_by_occurrence(count, occurrence, selected, error);

    if (line_hint > 0)
        return edc_edit_file_select_by_line_hint(content, content_len, count, line_nums, positions, old_len, line_hint,
                                                 selected, error);

    if (count > 1)
    {
        sds msg = sdsempty();
        edc_edit_file_format_match_list(content, content_len, positions, line_nums, old_len, count, 5, &msg);
        edc_error_set(error, EDC_ERR_EXEC,
                      "Warning: old_text appears %u times. "
                      "Provide more context, set occurrence, or set replace_all=true.%s",
                      count, msg);
        sdsfree(msg);
        return false;
    }

    {
        unsigned int zero = 0;
        utarray_push_back(*selected, &zero);
    }
    return true;
}

static void edc_edit_file_apply_replacements(const char *content, size_t content_len, const char *old_text,
                                             size_t old_len, const char *new_text, UT_array *positions,
                                             UT_array *selected, sds *out_content)
{
    size_t new_len = strlen(new_text);
    *out_content = sdsempty();
    size_t prev_end = 0;

    for (unsigned int si = 0; si < utarray_len(selected); si++)
    {
        unsigned int idx = *(unsigned int *)utarray_eltptr(selected, si);
        size_t start = *(size_t *)utarray_eltptr(positions, (unsigned)idx);
        size_t end = start + old_len;

        *out_content = sdscatlen(*out_content, content + prev_end, (ssize_t)(start - prev_end));
        *out_content = sdscat(*out_content, new_text);
        prev_end = end;

        if (new_len == 0 && end < content_len && content[end - 1] != '\n' && content[end] == '\n')
            prev_end++;
    }
    *out_content = sdscatlen(*out_content, content + prev_end, (ssize_t)(content_len - prev_end));
}

/* edit_file's I/O half. All 7 arguments were copied into the job on the runner -- the pool thread must not read args (§4.4). */
static sds edc_tool_edit_file_io(struct fs_job *job, EdcError **error)
{
    /* Local names match the old implementation, so the whole body below is unchanged. */
    const char *path = job->arg_path;
    sds safe_path = job->safe_path;
    const char *old_text = job->old_text;
    const char *new_text = job->new_text;
    bool replace_all = job->replace_all;
    int64_t occurrence = job->occurrence;
    int64_t line_hint = job->line_hint;
    int64_t expected_replacements = job->expected_replacements;

    sds created = edc_edit_file_try_create(path, safe_path, old_text, new_text, error);
    if (created || *error)
    {
        /* safe_path is the JOB's, freed by edc_fs_after_cb -- like the cleanup path below
         * says. It was freed here as well, so every edit_file that took this early return
         * ("Successfully created ...", "File not found", "already exists and is not empty")
         * released the same sds twice and aborted the process with "free(): invalid pointer". */
        return created;
    }

    sds content = NULL;
    UT_array *positions = NULL;
    UT_array *line_nums = NULL;
    UT_array *selected = NULL;
    sds result = NULL;

    EdcError *gerror = NULL;
    bool had_bom = false;
    {
        size_t size = 0;
        /* Through the TOOL reader, not edc_file_read_capped(): only that one strips a leading
         * BOM and reports it, and the report is what puts it back on write. Reading with the
         * raw reader left had_bom permanently false, so the restore branch below was dead
         * code and an edit silently dropped the BOM of any file that had one -- which on
         * Windows is most files PowerShell created. It also means old_text now matches the
         * same text read_file shows, which is the reason the strip exists at all. */
        content = fs_read_for_tool_capped_sized(safe_path, EDC_READ_MAX_BYTES_TOTAL, &size, &had_bom, &gerror);
        if (!content)
        {
            if (size > EDC_READ_MAX_BYTES_TOTAL)
                /* No paging here on purpose: old_text must be matched against the WHOLE file,
                 * so a partial read would silently miss (or hit the wrong) region. Refusing is
                 * the honest answer. */
                edc_error_set(error, EDC_ERR_INVAL,
                              "Error: '%s' is %zu bytes, over the %zu-byte limit for an edit. old_text has to "
                              "be matched against the whole file, so this tool cannot work piecewise; use "
                              "exec_shell (sed/perl) for a change this size.",
                              path ? path : "(file)", size, EDC_READ_MAX_BYTES_TOTAL);
            else
                edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
            edc_error_free(gerror);
            goto cleanup;
        }
    }
    size_t content_len = sdslen(content);

    size_t old_len = strlen(old_text);
    unsigned int count;
    edc_edit_file_find_matches(content, old_text, old_len, &positions, &line_nums, &count);

    if (count == 0)
    {
        edc_error_set(error, EDC_ERR_EXEC, "Error: old_text not found in %s", path);
        goto cleanup;
    }

    if (!edc_edit_file_select_matches(content, content_len, count, line_nums, positions, old_len, replace_all,
                                      occurrence, line_hint, &selected, error))
        goto cleanup;

    if (expected_replacements > 0 && utarray_len(selected) != (unsigned int)expected_replacements)
    {
        edc_error_set(error, EDC_ERR_EXEC, "Error: expected %ld replacements but would make %u.",
                      (long)expected_replacements, (unsigned)utarray_len(selected));
        goto cleanup;
    }

    sds new_content;
    edc_edit_file_apply_replacements(content, content_len, old_text, old_len, new_text, positions, selected,
                                     &new_content);

    /* An edit must not change bytes it was not asked to change: a file that had a BOM still has
     * one afterwards. sdscatsds copies its argument, so the old handle is released here. */
    if (had_bom)
    {
        sds with_bom = sdscatsds(sdsnewlen("\xEF\xBB\xBF", 3), new_content);
        sdsfree(new_content);
        new_content = with_bom;
    }

    if (!edc_file_write(safe_path, new_content, -1, error))
    {
        sdsfree(new_content);
    }
    else
    {
        sdsfree(new_content);
        result = sdscatprintf(sdsempty(), "Successfully edited %s", path);
    }

cleanup:
    if (content)
        sdsfree(content);
    /* safe_path is owned by the job and freed by after_cb -- do not free it here. */
    if (positions)
        utarray_free(positions);
    if (line_nums)
        utarray_free(line_nums);
    if (selected)
        utarray_free(selected);
    return result;
}

static void edc_tool_exec_edit_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                          void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    /* All 7 arguments are copied into the job on the runner -- the pool thread must not
     * read args (§4.4). Validation order matches the old implementation (missing arg ->
     * negative value -> sandbox). */
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    const char *old_text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "old_text"));
    const char *new_text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "new_text"));
    EdcError *error = NULL;
    if (!path || !old_text || !new_text)
    {
        edc_error_set(&error, EDC_ERR_EXEC, "Error: missing required parameter 'path', 'old_text', or 'new_text'.");
        callback(ctx, NULL, error);
        return;
    }

    bool replace_all = cJSON_IsTrue(cJSON_GetObjectItem(args, "replace_all"));

    cJSON *j;
    int64_t occurrence = 0;
    j = cJSON_GetObjectItem(args, "occurrence");
    if (j && cJSON_IsNumber(j))
        occurrence = (int64_t)j->valuedouble;

    int64_t line_hint = 0;
    j = cJSON_GetObjectItem(args, "line_hint");
    if (j && cJSON_IsNumber(j))
        line_hint = (int64_t)j->valuedouble;

    int64_t expected_replacements = 0;
    j = cJSON_GetObjectItem(args, "expected_replacements");
    if (j && cJSON_IsNumber(j))
        expected_replacements = (int64_t)j->valuedouble;

    if (occurrence < 0 || line_hint < 0 || expected_replacements < 0)
    {
        edc_error_set(&error, EDC_ERR_EXEC,
                      "Error: occurrence, line_hint, and expected_replacements must be non-negative.");
        callback(ctx, NULL, error);
        return;
    }

    sds safe = edc_sandbox_verify_path_at(ctx->tb->conf, edc_scheduler_task_workdir(ctx->task),
                                          path, true, &error);
    if (!safe)
    {
        callback(ctx, NULL, error);
        return;
    }

    /* tool_name = NULL: edit_file never sends a tool message, unchanged. */
    struct fs_job *job = edc_fs_job_new(ctx, callback, edc_tool_edit_file_io, NULL, path, safe);
    job->old_text = sdsnew(old_text);
    job->new_text = sdsnew(new_text);
    job->replace_all = replace_all;
    job->occurrence = occurrence;
    job->line_hint = line_hint;
    job->expected_replacements = expected_replacements;
    uv_queue_work(ctx->tb->scheduler->loop, &job->req, edc_fs_work_cb, edc_fs_after_cb);
}

static void edc_tool_create_edit_file(struct edc_toolbox *tb)
{
    const char *name = "edit_file";
    const char *desc =
        "Perform a small, exact replacement in one file by replacing old_text with new_text. Use this for narrow text "
        "substitutions with old_text copied from read_file. For multi-file, structural, or generated code edits, "
        "prefer apply_patch. If old_text matches multiple times, provide more context or set occurrence, line_hint, "
        "replace_all, and expected_replacements. Shows closest-match diagnostics on failure.";

    struct edc_tool_param *path = edc_tool_param_new("path", edc_param_str_type("The file path to edit", NULL), true);
    struct edc_tool_param *old_text =
        edc_tool_param_new("old_text", edc_param_str_type("The text to find and replace", NULL), true);
    struct edc_tool_param *new_text =
        edc_tool_param_new("new_text", edc_param_str_type("The text to replace with", NULL), true);
    struct edc_tool_param *replace_all =
        edc_tool_param_new("replace_all", edc_param_bool_type("Replace all occurrences (default false)"), false);

    struct edc_tool_param *occurrence = edc_tool_param_new(
        "occurrence",
        edc_param_int_type("Optional 1-based occurrence to replace when old_text appears multiple times.", 1,
                           G_MAXINT64),
        false);

    struct edc_tool_param *line_hint = edc_tool_param_new(
        "line_hint", edc_param_int_type("Optional 1-based line hint used to choose the nearest match.", 1, G_MAXINT64),
        false);

    struct edc_tool_param *expected_replacements = edc_tool_param_new(
        "expected_replacements",
        edc_param_int_type("Optional guard for the number of replacements that must be made.", 1, G_MAXINT64), false);

    struct edc_tool_param *params[] = {
        path, old_text, new_text, replace_all, occurrence, line_hint, expected_replacements, NULL};
    struct edc_tool *tool = edc_tool_new_v(name, desc, params, edc_tool_exec_edit_file_async);
    edc_tool_set_snippet(tool, "Replace an exact snippet in a file (disambiguate when it repeats)");
    edc_tool_set_guidelines(tool, "Read the file first: old_text must match it byte for byte.");
    edc_toolbox_register(tb, tool);
}

void edc_tools_fs_register(struct edc_toolbox *tb)
{
    edc_tool_create_read_file(tb);
    edc_tool_create_write_file(tb);
    edc_tool_create_edit_file(tb);
    edc_tool_create_list_dir(tb);
}
