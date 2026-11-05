#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_sandbox.h"
#include "edc_scheduler.h"
#include "edc_toolbox.h"
#include "edc_tools.h"

#include "log.h"
#include "sds.h"

#include <inttypes.h> /* PRId64 / PRIu64 */
#include <ctype.h>
#include <string.h>

/* POSIX regex on every platform: the system <regex.h> on POSIX, and pcre2's POSIX wrapper
 * on Windows (which has no <regex.h>). Note the HEADER NAME differs -- pcre2 ships
 * `pcre2posix.h`, and it maps regcomp/regexec/regerror/regfree onto pcre2_* with macros, so
 * only the include changes, not the calls. The BUILD select wires the dependency; it cannot
 * make `<regex.h>` exist. */
#ifdef _WIN32
#include <pcre2posix.h>
#else
#include <regex.h>
#endif

/* ── grep / find, in C ──────────────────────────────────────────────────────
 *
 * Earlier versions ran `sh -c 'rg ... || grep ...'` through the shell tool. That worked,
 * but it borrowed a machine it did not need: pipes, a process group, timeouts, SIGPIPE
 * handling and shell quoting all exist for spawning, and none of them are needed if the
 * walk happens here. It also made the arguments shell-fragile (every pattern had to be
 * quoted) and the timeouts coarse. Doing it in C removes the subprocess, removes the
 * quoting question entirely, and lets the search stop the moment it has enough.
 *
 * Two rules this file must respect:
 *
 *  1. NEVER BLOCK THE RUNNER. Traversal and file reads happen on libuv's thread pool
 *     (uv_queue_work), exactly like the fs tools. Only the sandbox check and the final
 *     callback run on the runner.
 *
 *     Cancellation: the call context lives until the tool CALLS BACK (the actor frees it in
 *     its chain tail), so a cancelled search keeps its ctx valid -- it just runs on until a
 *     limit or the deadline and its result is discarded. No abort hook is registered for
 *     that reason: uv_work_t cannot be interrupted, and the timeout already bounds the
 *     overrun. (A hook could set a flag to end the walk early; not worth the state until
 *     searches on huge trees are measured to be a problem.)
 *  2. The sandbox check stays on the runner: it reads conf's ro/rw/deny tables, which
 *     /sandbox mutates on the runner thread. Only the VERIFIED absolute path crosses into
 *     the pool, so a `../..` argument cannot widen the search.
 *
 * Cross-platform notes, because this code has to work on Linux, macOS and Windows:
 *  - Directory iteration goes through edc_dir_foreach (FindFirstFile on Windows,
 *    opendir/readdir elsewhere).
 *  - `d_type` is DT_UNKNOWN on some filesystems, so the type falls back to a stat.
 *  - Symlinks are not followed (a cycle would never terminate); a depth cap backs that up
 *    for the platforms/filesystems that cannot report a link at all.
 *  - Text files may have CRLF line endings, so a trailing '\r' is stripped per line.
 *  - A file containing a NUL byte in its first 8 KiB is treated as binary and skipped.
 *  - The glob matcher is case-insensitive on Windows (its filesystems are) and
 *    case-sensitive elsewhere.
 *  - Paths in the result are relative to the search root, joined with '/', so the output
 *    looks the same to the model on every platform. */

#define EDC_SEARCH_MAX_LINE 500        /* characters kept per matching line */
#define EDC_SEARCH_DEFAULT_LIMIT 100   /* matches, when the caller names no limit */
#define EDC_SEARCH_MAX_FILE_BYTES (8u * 1024u * 1024u)
/* Total bytes of result text. `limit` bounds the number of matches, not their size: a model
 * that asks for limit=100000 in /usr produced ~9.8 MB, which would blow up the context it is
 * meant to inform. 64 KiB is far more than any useful answer and small enough to be safe. */
#define EDC_SEARCH_MAX_OUTPUT (64u * 1024u)
#define EDC_SEARCH_MAX_DEPTH 64

static const char *const kSkipDirs[] = {
    ".git",      ".hg",           ".svn",   "node_modules", "build", "bazel-out",
    "bazel-bin", "bazel-testlogs", "__pycache__", ".cache",  ".venv", "venv",
    "target",    ".idea",         ".vscode", "dist",        ".next", ".gradle",
};

static bool skip_dir_name(const char *name)
{
    for (size_t i = 0; i < sizeof(kSkipDirs) / sizeof(kSkipDirs[0]); i++)
        if (strcmp(name, kSkipDirs[i]) == 0)
            return true;
    return false;
}

#ifdef _WIN32
#define EDC_GLOB_FOLD(c) tolower((unsigned char)(c))
#else
#define EDC_GLOB_FOLD(c) (c)
#endif

/* Match `*` and `?` against |s|. `*` does not cross a '/' -- a glob filters within a
 * directory level unless the caller passed a path glob, which is the behaviour the
 * `path` argument already expresses. Recursive with backtracking on `*`. */
bool edc_search_glob_match(const char *glob, const char *s)
{
    if (!glob || !s)
        return false;
    if (!*glob)
        return true; /* an empty glob filters nothing */

    const char *g = glob, *p = s;
    const char *star = NULL, *resume = NULL;

    while (*p)
    {
        if (*g == '*')
        {
            star = g++;
            resume = p;
        }
        else if (*g == '?')
        {
            if (*p == '/')
                return false; /* ? is one character within a level */
            g++;
            p++;
        }
        else if (*g && EDC_GLOB_FOLD(*g) == EDC_GLOB_FOLD(*p))
        {
            /* A literal '/', including the one that follows a directory prefix, matches
             * itself -- only `*` and `?` are forbidden from crossing it. */
            g++;
            p++;
        }
        else if (star)
        {
            if (*resume == '/')
                return false; /* `*` must not swallow a directory separator */
            g = star + 1;
            p = ++resume;
        }
        else
            return false;
    }
    while (*g == '*')
        g++;
    return *g == '\0';
}

/* Case handling is the platform's: Windows filesystems fold case, POSIX ones do not. */
static bool glob_match_file(const char *glob, const char *name, const char *rel)
{
    if (!glob || !*glob)
        return true;
    /* A glob with a separator is matched against the relative path; otherwise against the
     * file name alone (so "*.c" means "a .c file anywhere in the tree"). */
    const bool has_dir = strchr(glob, '/') != NULL || strchr(glob, '\\') != NULL;
    return edc_search_glob_match(glob, has_dir ? rel : name);
}

/* ── literal search (no regex engine involved) ─────────────────────────────── */

static const char *find_literal(const char *hay, const char *needle, bool ignore_case)
{
    if (!hay || !needle || !*needle)
        return NULL;
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
    {
        size_t i = 0;
        while (i < n && p[i] && (ignore_case ? (tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
                                             : (p[i] == needle[i])))
            i++;
        if (i == n)
            return p;
    }
    return NULL;
}

/* ── the job ──────────────────────────────────────────────────────────────── */

struct search_job
{
    uv_work_t req;
    struct edc_toolbox_call_ctx *ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *);

    /* Copied on the runner; `args` must never be touched from the pool. */
    bool is_grep;
    sds root; /* verified absolute path */
    sds header;
    sds pattern;
    sds glob;
    bool ignore_case;
    bool literal;
    int64_t limit;
    uint64_t deadline_ns;

    /* Filled on the pool thread. */
    sds out;
    int64_t matches;
    bool truncated;   /* stopped because of |limit| */
    bool timed_out;   /* stopped because of the deadline */
    bool out_capped;  /* stopped because the result text hit EDC_SEARCH_MAX_OUTPUT */
    int files_scanned;
    EdcError *error;
};

static void job_free(struct search_job *job)
{
    if (!job)
        return;
    sdsfree(job->root);
    sdsfree(job->header);
    sdsfree(job->pattern);
    sdsfree(job->glob);
    if (job->out)
        sdsfree(job->out);
    if (job->error)
        edc_error_free(job->error);
    free(job);
}

/* Should the walk stop now? Checked per directory entry, so a --limit or a deadline ends
 * the search promptly instead of after the whole tree. */
static bool job_stop(struct search_job *job)
{
    if (job->out_capped)
        return true;
    if (job->limit > 0 && job->matches >= job->limit)
    {
        job->truncated = true;
        return true;
    }
    if (job->deadline_ns && uv_hrtime() > job->deadline_ns)
    {
        job->timed_out = true;
        return true;
    }
    return false;
}

/* The path as the model should see it: relative to the search root, '/'-separated. */
static sds relative_path(const struct search_job *job, const char *full)
{
    const size_t root_len = sdslen(job->root);
    const char *rel = full;
    if (strncmp(full, job->root, root_len) == 0)
    {
        rel = full + root_len;
        while (*rel == '/' || *rel == '\\')
            rel++;
    }
    sds out = sdsnew(rel);
    for (size_t i = 0; i < sdslen(out); i++)
        if (out[i] == '\\')
            out[i] = '/';
    return out;
}

static void append_hit(struct search_job *job, const char *full, int lineno, const char *line, size_t len)
{
    sds rel = relative_path(job, full);

    /* One pathological line must not decide the size of the result. */
    size_t keep = len;
    bool cut = false;
    if (keep > EDC_SEARCH_MAX_LINE)
    {
        keep = EDC_SEARCH_MAX_LINE;
        cut = true;
    }

    sds piece = sdscatprintf(sdsempty(), "%s:%d:", rel, lineno);
    piece = sdscatlen(piece, line, keep);
    if (cut)
        piece = sdscat(piece, " ... [line truncated]");
    piece = sdscat(piece, "\n");

    /* Checked before appending, so a line is never half-written. */
    if (job->out && sdslen(job->out) + sdslen(piece) > EDC_SEARCH_MAX_OUTPUT)
    {
        job->out_capped = true;
        sdsfree(piece);
        sdsfree(rel);
        return;
    }
    if (!job->out)
        job->out = sdsempty();
    job->out = sdscatsds(job->out, piece);
    sdsfree(piece);
    sdsfree(rel);
    job->matches++;
}

/* Scan one file for matches. Runs on the pool thread. */
static void grep_file(struct search_job *job, const char *full, regex_t *re)
{
    if (job_stop(job))
        return;

    /* The size ceiling is enforced by the reader itself (before it allocates), so a 2 GB file
     * in the tree costs one stat instead of 2 GB of memory -- and there is no second place
     * that has to remember to check. */
    EdcError *gerror = NULL;
    sds content = edc_file_read_capped(full, EDC_SEARCH_MAX_FILE_BYTES, NULL, &gerror);
    if (!content)
    {
        if (gerror)
            edc_error_free(gerror);
        return; /* unreadable (permissions, raced deletion) is not an error of the search */
    }
    job->files_scanned++;

    /* The whole buffer, not a prefix: `regexec`/`find_literal` stop at a NUL, so a NUL
     * further in would silently end that line's match candidate -- a false negative with no
     * trace in the output. Reading the file is already done; this second pass is cheap. */
    if (memchr(content, '\0', sdslen(content)) != NULL)
    {
        sdsfree(content); /* binary: no useful lines to show */
        return;
    }

    int lineno = 0;
    const char *p = content;
    const char *end = content + sdslen(content);
    while (p < end && !job_stop(job))
    {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        /* CRLF: the '\r' belongs to the line ending, not to the text. */
        size_t len = (size_t)(line_end - p);
        if (len > 0 && p[len - 1] == '\r')
            len--;
        lineno++;

        sds line = sdsnewlen(p, len);
        bool hit;
        if (job->literal)
            hit = find_literal(line, job->pattern, job->ignore_case) != NULL;
        else
            hit = regexec(re, line, 0, NULL, 0) == 0;
        if (hit)
            append_hit(job, full, lineno, line, len);
        sdsfree(line);

        if (!nl)
            break;
        p = nl + 1;
    }
    sdsfree(content);
}

/* edc_dir_foreach's callback carries no "which directory am I in", so each level passes
 * its own frame through the userdata pointer. (A global would race: several searches can
 * run on different pool threads at once.) */
struct dir_frame
{
    struct search_job *job;
    regex_t *re;
    sds dir;
    int depth;
};

static void on_entry(const char *name, int type, void *ud)
{
    struct dir_frame *f = (struct dir_frame *)ud;
    struct search_job *job = f->job;
    if (job_stop(job))
        return;
    /* edc_dir_foreach does not filter these (POSIX readdir hands them out). */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return;
    /* A symlink is not followed: a link to an ancestor would make the walk never end. */
    if (type == UV_DIRENT_LINK)
        return;

    sds full = edc_path_join(f->dir, name, NULL);
    /* DT_UNKNOWN happens on some filesystems (and on Windows for anything unusual), so
     * everything the dirent does not classify falls back to a stat. FIFOs, sockets and
     * devices end up "not a dir, not a regular file" below and are skipped. */
    bool is_dir;
    switch (type)
    {
    case UV_DIRENT_DIR:
        is_dir = true;
        break;
    case UV_DIRENT_FILE:
        is_dir = false;
        break;
    default:
        is_dir = edc_file_is_dir(full);
        break;
    }

    if (is_dir)
    {
        if (!skip_dir_name(name) && f->depth < EDC_SEARCH_MAX_DEPTH)
        {
            struct dir_frame child = {.job = job, .re = f->re, .dir = full, .depth = f->depth + 1};
            edc_dir_foreach(full, on_entry, &child, NULL);
        }
        sdsfree(full);
        return;
    }

    /* A dirent that already says "regular file" needs no second stat; only the ambiguous
     * cases do (FIFOs, sockets, devices, DT_UNKNOWN). */
    if (type != UV_DIRENT_FILE && !edc_file_is_regular(full))
    {
        sdsfree(full);
        return;
    }

    sds rel = relative_path(job, full);
    if (glob_match_file(job->glob, name, rel))
    {
        if (job->is_grep)
        {
            grep_file(job, full, f->re);
        }
        else if (job->matches < job->limit)
        {
            /* find: the glob IS the filter, and there is no content to read. Same total
             * budget as grep -- a path list can be just as long as a match list. */
            sds piece = sdscatprintf(sdsempty(), "%s\n", rel);
            if (job->out && sdslen(job->out) + sdslen(piece) > EDC_SEARCH_MAX_OUTPUT)
                job->out_capped = true;
            else
            {
                if (!job->out)
                    job->out = sdsempty();
                job->out = sdscatsds(job->out, piece);
                job->matches++;
                job->files_scanned++;
            }
            sdsfree(piece);
        }
    }
    sdsfree(rel);
    sdsfree(full);
}

/* Take the collected output and add the "why it ended" trailer. Shared by the async
 * callback and the blocking entry point so a caller cannot get one without the other. */
/* Take the collected output and put the "why it ended" notices at the HEAD.
 *
 * Head, not tail: a result long enough to be capped is cut from the front by the toolbox (or
 * by the preview in front of it), so anything appended at the end is exactly the part nobody
 * sees. This was not theoretical -- the 64 KiB note used to sit at the tail and never reached
 * the model at all, because the toolbox's 10 KB cap cut it away. */
static sds search_finish(struct search_job *job)
{
    sds body = job->out ? job->out : sdsempty();
    job->out = NULL;

    sds notes = sdsempty();
    if (job->truncated)
        notes = sdscatprintf(notes,
                             "[reached the limit of %" PRId64 " matches -- there may be more; raise limit or narrow "
                             "the pattern]\n",
                             job->limit);
    else if (job->timed_out)
        notes = sdscat(notes, "[stopped at the timeout; narrow the path or raise the timeout]\n");
    if (job->out_capped)
        notes = sdscatprintf(notes, "[output capped at %u KiB; narrow the pattern or the path]\n",
                             (unsigned)(EDC_SEARCH_MAX_OUTPUT / 1024));

    if (sdslen(body) == 0 && !job->error)
    {
        if (job->is_grep)
            body = sdscatprintf(body, "(no matches; %d files read)\n", job->files_scanned);
        else
            body = sdscat(body, "(no files matched)\n");
    }

    sds result = notes;
    result = sdscatsds(result, body);
    sdsfree(body);
    return result;
}

static void run_search(struct search_job *job)
{
    regex_t re;
    regex_t *rep = NULL;
    if (job->is_grep && !job->literal)
    {
        int flags = REG_EXTENDED | REG_NOSUB;
        if (job->ignore_case)
            flags |= REG_ICASE;
        if (regcomp(&re, job->pattern, flags) != 0)
        {
            job->error = edc_error_new(EDC_ERR_INVAL, "grep: invalid pattern '%s'", job->pattern);
            return;
        }
        rep = &re;
    }

    struct dir_frame root = {.job = job, .re = rep, .dir = job->root, .depth = 0};
    edc_dir_foreach(job->root, on_entry, &root, NULL);

    if (rep)
        regfree(&re);
}

/* Synchronous entry point: the same walk, on the caller's thread. The tool path calls it
 * from the pool (never from the runner); tests call it directly so a search can be checked
 * without a running actor system. Returns the result text (never NULL). */
sds edc_search_run_blocking(const char *root, bool is_grep, const char *pattern, const char *glob, bool ignore_case,
                            bool literal, int64_t limit)
{
    struct search_job job;
    memset(&job, 0, sizeof(job));
    job.is_grep = is_grep;
    job.root = sdsnew(root);
    job.pattern = sdsnew(pattern ? pattern : "");
    job.glob = (glob && *glob) ? sdsnew(glob) : NULL;
    job.ignore_case = ignore_case;
    job.literal = literal;
    job.limit = limit > 0 ? limit : EDC_SEARCH_DEFAULT_LIMIT;

    run_search(&job);

    sds out;
    if (job.error)
    {
        /* A failed search has no text; do not manufacture "(no matches)" for it. */
        if (job.out)
        {
            sdsfree(job.out);
            job.out = NULL;
        }
        edc_error_free(job.error);
        out = sdsempty();
    }
    else
        out = search_finish(&job);
    sdsfree(job.root);
    sdsfree(job.pattern);
    sdsfree(job.glob);
    return out;
}

/* ── pool / runner plumbing ───────────────────────────────────────────────── */

static void search_work_cb(uv_work_t *req)
{
    struct search_job *job = (struct search_job *)req->data;
    run_search(job);
}

static void search_after_cb(uv_work_t *req, int status)
{
    (void)status;
    struct search_job *job = (struct search_job *)req->data;

    if (job->error)
    {
        /* A failed search has no result text -- the error says what went wrong, and partial
         * output would contradict it. */
        if (job->out)
        {
            sdsfree(job->out);
            job->out = NULL;
        }
        EdcError *error = job->error;
        job->error = NULL;
        job->callback(job->ctx, NULL, error);
        job_free(job);
        return;
    }

    /* search_finish hands the collected text over, and ownership then moves to the callback
     * -- the same contract every other tool follows (see edc_fs_after_cb: it never frees
     * job->result after calling back). Freeing it here too was the crash: the actor took the
     * string, this function freed it again, and the reader saw a dangling pointer. */
    sds result = search_finish(job);
    if (job->header)
        edc_toolbox_emit_tool_message_plain(job->ctx, job->header, result);
    job->callback(job->ctx, result, NULL);
    job_free(job);
}

static unsigned int search_timeout(cJSON *args)
{
    unsigned int timeout = 30;
    cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "timeout");
    if (t && cJSON_IsNumber(t) && t->valueint > 0)
        timeout = (unsigned int)t->valueint;
    return timeout;
}

static int64_t search_limit(cJSON *args)
{
    cJSON *l = cJSON_GetObjectItemCaseSensitive(args, "limit");
    if (l && cJSON_IsNumber(l) && l->valueint > 0)
        return (int64_t)l->valueint;
    return EDC_SEARCH_DEFAULT_LIMIT;
}

/* Runner half: sandbox check, copy the arguments, hand the work to the pool. */
static void search_start(struct edc_toolbox_call_ctx *ctx, bool is_grep, cJSON *args,
                         void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    const char *workdir = edc_scheduler_task_workdir(ctx->task);
    if (!workdir)
        workdir = edc_sandbox_get_root(ctx->tb->conf);

    EdcError *error = NULL;
    sds root = edc_sandbox_verify_path_at(ctx->tb->conf, workdir, path ? path : ".", false, &error);
    if (!root)
    {
        callback(ctx, NULL, error);
        return;
    }
    if (!edc_file_is_dir(root))
    {
        sdsfree(root);
        callback(ctx, NULL, edc_error_new(EDC_ERR_INVAL, "not a directory: %s", path ? path : "."));
        return;
    }

    struct search_job *job = calloc(1, sizeof(*job));
    if (!job)
    {
        sdsfree(root);
        callback(ctx, NULL, edc_error_new(EDC_ERR_FAILED, "out of memory"));
        return;
    }
    job->req.data = job;
    job->ctx = ctx;
    job->callback = callback;
    job->is_grep = is_grep;
    job->root = root;

    const char *pattern = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "pattern"));
    const char *glob = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "glob"));
    cJSON *ci = cJSON_GetObjectItemCaseSensitive(args, "ignore_case");
    cJSON *lit = cJSON_GetObjectItemCaseSensitive(args, "literal");

    job->pattern = sdsnew(pattern ? pattern : "");
    job->glob = glob && *glob ? sdsnew(glob) : NULL;
    job->ignore_case = ci && cJSON_IsTrue(ci);
    job->literal = lit && cJSON_IsTrue(lit);
    job->limit = search_limit(args);
    job->deadline_ns = uv_hrtime() + (uint64_t)search_timeout(args) * 1000000000ull;
    job->header = sdscatprintf(sdsempty(), "%s %s%s%s", is_grep ? "grep" : "find",
                               is_grep ? job->pattern : (job->glob ? job->glob : "*"),
                               path ? " in " : "", path ? path : ".");

    uv_queue_work(ctx->tb->scheduler->loop, &job->req, search_work_cb, search_after_cb);
}

static void edc_tool_exec_grep_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                     void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    const char *pattern = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "pattern"));
    if (!pattern || !*pattern)
    {
        callback(ctx, NULL, edc_error_new(EDC_ERR_INVAL, "grep needs a pattern"));
        return;
    }
    search_start(ctx, true, args, callback);
}

static void edc_tool_exec_find_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                     void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    search_start(ctx, false, args, callback);
}

void edc_tools_search_register(struct edc_toolbox *tb)
{
    {
        struct edc_tool_param *params = NULL;
        LL_APPEND(params, edc_tool_param_new("pattern", edc_param_str_type("Search pattern (POSIX ERE, or a literal string when `literal` is set)", NULL), true));
        LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("Directory to search (default: the session's directory)", NULL), false));
        LL_APPEND(params, edc_tool_param_new("glob", edc_param_str_type("Only search files matching this glob, e.g. \"*.c\"", NULL), false));
        LL_APPEND(params, edc_tool_param_new("ignore_case", edc_param_bool_type("Case-insensitive match"), false));
        LL_APPEND(params, edc_tool_param_new("literal", edc_param_bool_type("Treat the pattern as a literal string, not a regex"), false));
        LL_APPEND(params, edc_tool_param_new("limit", edc_param_int_type("Maximum matches to return (default 100)", 1, 100000), false));
        LL_APPEND(params, edc_tool_param_new("timeout", edc_param_int_type("Timeout in seconds (default 30)", 1, 3600), false));

        struct edc_tool *tool = edc_tool_new("grep",
                                             "Search file contents for a pattern. Returns matching lines as "
                                             "path:line:text, relative to the searched directory. Skips binary "
                                             "files, VCS metadata and build output.",
                                             params, edc_tool_exec_grep_async);
        edc_tool_set_snippet(tool, "Search file contents for a pattern");
        edc_tool_set_guidelines(tool, "Prefer this over exec_shell for searching.\n"
                                      "Stops at `limit` matches (default 100) and says so; narrow the pattern "
                                      "instead of re-running.");
        edc_toolbox_register(tb, tool);
    }
    {
        struct edc_tool_param *params = NULL;
        LL_APPEND(params, edc_tool_param_new("glob", edc_param_str_type("File name glob, e.g. \"*.c\" (default: every file)", NULL), false));
        LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("Directory to walk (default: the session's directory)", NULL), false));
        LL_APPEND(params, edc_tool_param_new("limit", edc_param_int_type("Maximum paths to return (default 100)", 1, 100000), false));
        LL_APPEND(params, edc_tool_param_new("timeout", edc_param_int_type("Timeout in seconds (default 30)", 1, 3600), false));

        struct edc_tool *tool = edc_tool_new("find",
                                             "List files under a directory, optionally filtered by a name glob. "
                                             "Skips VCS metadata and build output.",
                                             params, edc_tool_exec_find_async);
        edc_tool_set_snippet(tool, "Find files by name glob");
        edc_tool_set_guidelines(tool, "Prefer this over `exec_shell find`: same walk, without the noise.\n"
                                      "Stops at `limit` paths (default 100) or 64 KiB of output, and says "
                                      "which one ended it.");
        edc_toolbox_register(tb, tool);
    }
}
