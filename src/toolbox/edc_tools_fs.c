#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_sandbox.h"
#include "edc_scheduler.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "sds.h"
#include "utarray.h"
#include <edc_param_types.h>
#include <string.h>

static UT_icd ut_size_t_icd = {sizeof(size_t), NULL, NULL, NULL};
static UT_icd ut_int64_icd = {sizeof(int64_t), NULL, NULL, NULL};
static UT_icd ut_uint_icd = {sizeof(unsigned int), NULL, NULL, NULL};

/* ── fs I/O 的异步外壳 ────────────────────────────────────────
 *
 * 为什么需要: 文件 I/O 是阻塞的, 而工具是在 **runner 线程**上被执行的 —— 一次慢
 * I/O (大文件 / 冷缓存 / NFS) 会冻住**所有** actor, 不只是发起它的那个 task。
 * 这违反 runtime-model.md §2.9 的架构禁令「runner 线程绝不阻塞」。
 *
 * 拆法 (见 docs/fs-io-off-runner.md):
 *   runner:  沙箱判定 (读 conf 的 ro/rw/deny 三张表) -> 已校验的绝对路径
 *   池线程:  纯文件 I/O  (uv_fs_*(..., NULL) 的同步路径**不使用线程池**, 它就在
 *            调用者线程上做系统调用 —— 于是池线程上调用, I/O 就发生在池线程)
 *   runner:  after_cb -> 发工具消息 + 调 callback
 *
 * 沙箱判定**必须留在 runner**: /sandbox add-* 也在 runner 线程上改那三张表, 把
 * "校验 + I/O"整段丢进池线程会新生一条数据竞争 (今天没有的)。
 *
 * 两条生命周期规矩:
 *   1. **args 不能进池**。exec 返回后工具箱就释放了 req (连同 args), 而异步工具的
 *      exec 是立刻返回的 —— 池线程读 args 就是 use-after-free。凡入池要用的都在
 *      runner 上抄进 job (§4.4)。
 *   2. **ctx 反而安全**。call_ctx 由工具箱的 inflight 登记表管, 它正是在 callback
 *      里才被 free —— 跨这一跳持有它是架构本来就支持的 (edc_toolbox_actor.c 末尾
 *      那句 `// TODO: async exec` 就是给这个留的位置, shell 已经在这么用)。
 *
 * 顺带 (不改行为): 池默认 4 条。4 个 fs 操作同时卡住会让后续 fs 操作排队, 但 runner
 * 活着 —— 那正是本改动的目标。不给 fs 加 deadline: 池线程无法被中断。 */
struct fs_job
{
    uv_work_t req;
    struct edc_toolbox_call_ctx *ctx;
    void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *);
    sds (*io)(struct fs_job *, EdcError **); /* 池线程上执行 */
    sds safe_path;                           /* runner 上算好; 池线程只读 */
    sds arg_path;                            /* 原始 path, 只用于回包文案 */
    sds header;                              /* 非 NULL 才发工具消息 (edit_file 不发, 保持原样) */
    sds content;                             /* write_file 的正文 */
    sds old_text;                            /* edit_file */
    sds new_text;
    bool replace_all;
    int64_t occurrence;
    int64_t line_hint;
    int64_t expected_replacements;
    sds result; /* 池线程填 */
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

/* runner 半段: 组装 job。tool_name 为 NULL -> 不发工具消息 (edit_file 本来就不发)。 */
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

/* runner 半段: 沙箱判定 -> 入池。失败则直接在 runner 上回错 (与原来同步失败同路)。 */
static void edc_fs_kick_path_tool(struct edc_toolbox_call_ctx *ctx, cJSON *args, const char *tool_name, bool write,
                                  void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *),
                                  sds (*io)(struct fs_job *, EdcError **))
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
    uv_queue_work(ctx->tb->scheduler->loop, &edc_fs_job_new(ctx, callback, io, tool_name, path, safe)->req,
                  edc_fs_work_cb, edc_fs_after_cb);
}

/*
 * ↓↓ 以下是四个工具的 I/O 半段: 只吃 job, **不碰 conf、不碰 args**。
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

static sds edc_tool_read_file_io(struct fs_job *job, EdcError **error)
{
    EdcError *gerror = NULL;
    sds content = edc_file_read(job->safe_path, &gerror);
    if (!content)
    {
        edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
        edc_error_free(gerror);
        return NULL;
    }
    return content;
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


static void edc_tool_exec_read_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                          void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    edc_fs_kick_path_tool(ctx, args, "read_file", false, callback, edc_tool_read_file_io);
}

static void edc_tool_create_read_file(struct edc_toolbox *tb)
{
    const char *name = "read_file";
    const char *desc = "Read the contents of a file at the given path.";
    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("The file path to read", NULL), true));
    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_read_file_async));
}

static void edc_tool_exec_write_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                           void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    /* content 必须在 runner 上抄一份: exec 返回后 args 就被释放 (§4.4)。 */
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

    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_write_file_async));
}

static void edc_tool_exec_list_dir_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                         void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    (void)tool;
    edc_fs_kick_path_tool(ctx, args, "list_dir", false, callback, edc_tool_list_dir_io);
}

static void edc_tool_create_list_dir(struct edc_toolbox *tb)
{
    const char *name = "list_dir";
    const char *desc = "List the contents of a directory at the given path.";

    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("The directory path to list", NULL), true));
    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_list_dir_async));
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
        sds content = edc_file_read(safe_path, &gerror);
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

/* edit_file 的 I/O 半段。7 个参数已在 runner 上抄进 job —— 池线程不许读 args (§4.4)。 */
static sds edc_tool_edit_file_io(struct fs_job *job, EdcError **error)
{
    /* 局部名与原实现保持一致, 于是下面那整段本体一字未改。 */
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
        sdsfree(safe_path);
        return created;
    }

    sds content = NULL;
    UT_array *positions = NULL;
    UT_array *line_nums = NULL;
    UT_array *selected = NULL;
    sds result = NULL;

    EdcError *gerror = NULL;
    content = edc_file_read(safe_path, &gerror);
    if (!content)
    {
        edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
        edc_error_free(gerror);
        goto cleanup;
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
    /* safe_path 归 job 所有, 由 after_cb 释放 —— 这里不能 free。 */
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
    /* 7 个参数全部在 runner 上抄进 job —— 池线程不许读 args (§4.4)。
     * 校验顺序与原实现一致 (缺参 -> 负值 -> 沙箱)。 */
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

    /* tool_name = NULL: edit_file 本来就不发工具消息, 保持原样。 */
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
    edc_toolbox_register(tb, tool);
}

void edc_tools_fs_register(struct edc_toolbox *tb)
{
    edc_tool_create_read_file(tb);
    edc_tool_create_write_file(tb);
    edc_tool_create_edit_file(tb);
    edc_tool_create_list_dir(tb);
}
