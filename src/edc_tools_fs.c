#include "sds.h"
#include "edc_error.h"
#include "edc_sandbox.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include <edc_param_types.h>
#include <string.h>
#include "utarray.h"

static UT_icd ut_size_t_icd = {sizeof(size_t), NULL, NULL, NULL};
static UT_icd ut_int64_icd = {sizeof(int64_t), NULL, NULL, NULL};
static UT_icd ut_uint_icd = {sizeof(unsigned int), NULL, NULL, NULL};

static sds edc_tool_exec_read_file(struct edc_config *conf, cJSON *args, EdcError **error)
{
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));

    sds safe_path = edc_sandbox_verify_path(conf, path, FALSE, error);
    if (!safe_path)
    {
        return NULL;
    }

    char *content;
    size_t len = 0;
    GError *gerror = NULL;
    g_file_get_contents(safe_path, &content, &len, &gerror);
    sdsfree(safe_path);
    if (gerror)
    {
        edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
        g_error_free(gerror);
        return NULL;
    }

    sds result = sdsnewlen(content, len);
    free(content);
    return result;
}

static sds edc_tool_exec_write_file(struct edc_config *conf, cJSON *args, EdcError **error)
{
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "content"));

    sds safe_path = edc_sandbox_verify_path(conf, path, TRUE, error);
    if (!safe_path)
    {
        return NULL;
    }

    GError *gerror = NULL;
    g_file_set_contents(safe_path, content, -1, &gerror);
    sdsfree(safe_path);
    if (gerror)
    {
        edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
        g_error_free(gerror);
        return NULL;
    }

    sds result = sdsnew("");
    result = sdscatprintf(result, "Successfully wrote to file %s", path);
    return result;
}

static sds edc_tool_exec_list_dir(struct edc_config *conf, cJSON *args, EdcError **error)
{
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));

    sds safe_path = edc_sandbox_verify_path(conf, path, FALSE, error);
    if (!safe_path)
    {
        return NULL;
    }

    GError *gerror = NULL;
    GDir *dir = g_dir_open(safe_path, 0, &gerror);
    sdsfree(safe_path);
    if (gerror)
    {
        edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
        g_error_free(gerror);
        return NULL;
    }

    sds result = sdsnew("");
    const char *name;
    while ((name = g_dir_read_name(dir)))
    {
        char *full_path = g_build_filename(path, name, NULL);
        bool is_dir = g_file_test(full_path, G_FILE_TEST_IS_DIR);
        free(full_path);

        const char *prefix = is_dir ? "[DIR]  " : "[FILE] ";
        result = sdscatprintf(result, "%s%s\n", prefix, name);
    }

    g_dir_close(dir);
    return result;
}

static void edc_tool_exec_read_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                          void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{
    EdcError *error = NULL;
    sds result = edc_tool_exec_read_file(ctx->tb->conf, args, &error);
    callback(ctx, result, error);
}

static void edc_tool_create_read_file(struct edc_toolbox *tb)
{
    const char *name = "read_file";
    const char *desc = "Read the contents of a file at the given path.";
    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("The file path to read", NULL), TRUE));
    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_read_file_async));
}

static void edc_tool_exec_write_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                           void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{
    EdcError *error = NULL;
    sds result = edc_tool_exec_write_file(ctx->tb->conf, args, &error);
    callback(ctx, result, error);
}

static void edc_tool_create_write_file(struct edc_toolbox *tb)
{
    const char *name = "write_file";
    const char *desc = "Write the contents of a file at the given path.";

    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("The file path to write", NULL), TRUE));
    LL_APPEND(params, edc_tool_param_new("content", edc_param_str_type("The content to write", NULL), TRUE));

    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_write_file_async));
}

static void edc_tool_exec_list_dir_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                         void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{
    EdcError *error = NULL;
    sds result = edc_tool_exec_list_dir(ctx->tb->conf, args, &error);
    callback(ctx, result, error);
}

static void edc_tool_create_list_dir(struct edc_toolbox *tb)
{
    const char *name = "list_dir";
    const char *desc = "List the contents of a directory at the given path.";

    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("path", edc_param_str_type("The directory path to list", NULL), TRUE));
    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_list_dir_async));
}

static sds edc_edit_file_try_create(const char *path, sds safe_path,
                                            const char *old_text, const char *new_text,
                                            EdcError **error)
{
    if (!g_file_test(safe_path, G_FILE_TEST_EXISTS))
    {
        if (old_text && old_text[0] == '\0')
        {
            char *parent = g_path_get_dirname(safe_path);
            g_mkdir_with_parents(parent, 0755);
            free(parent);
            GError *gerror = NULL;
            g_file_set_contents(safe_path, new_text, -1, &gerror);
            if (gerror)
            {
                edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
                g_error_free(gerror);
                return NULL;
            }
            sds msg = sdscatprintf(sdsempty(), "Successfully created %s", path);
            return msg;
        }
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: File not found: %s", path);
        return NULL;
    }

    if (old_text && old_text[0] == '\0')
    {
        char *content;
        GError *gerror = NULL;
        g_file_get_contents(safe_path, &content, NULL, &gerror);
        if (gerror)
        {
            edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
            g_error_free(gerror);
            return NULL;
        }
        sds sds_content = sdsnewlen(content, strlen(content));
        free(content);
        sds_content = sdstrim(sds_content, " \t\r\n");
        if (sds_content[0] != '\0')
        {
            sdsfree(sds_content);
            edc_error_set(error, EDC_ERR_EXEC,
                        "Error: Cannot create file — %s already exists and is not empty.", path);
            return NULL;
        }
        sdsfree(sds_content);
        GError *gerror2 = NULL;
        g_file_set_contents(safe_path, new_text, -1, &gerror2);
        if (gerror2)
        {
            edc_error_set(error, EDC_ERR_FAILED, "%s", gerror2->message);
            g_error_free(gerror2);
            return NULL;
        }
        sds msg = sdscatprintf(sdsempty(), "Successfully edited %s", path);
        return msg;
    }

    return NULL;
}

static void edc_edit_file_find_matches(const char *content, const char *old_text, size_t old_len,
                                        UT_array **positions, UT_array **line_nums, unsigned int *out_count)
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

static void edc_edit_file_format_match_list(const char *content, size_t content_len,
                                              UT_array *positions, UT_array *line_nums,
                                              size_t old_len, unsigned int count, unsigned int max_preview,
                                              sds *out)
{
    unsigned int preview = MIN(count, max_preview);
    for (unsigned int i = 0; i < preview; i++)
    {
        size_t pos = *(size_t*)utarray_eltptr(positions, (unsigned)i);
        int64_t ln = *(int64_t*)utarray_eltptr(line_nums, (unsigned)i);
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
    return TRUE;
}

static bool edc_edit_file_select_by_occurrence(unsigned int count, int64_t occurrence,
                                                     UT_array **selected, EdcError **error)
{
    if ((uint64_t)occurrence > count)
    {
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: occurrence %ld out of range; old_text appears %u times.",
                    (long)occurrence, count);
        return FALSE;
    }
    unsigned int idx = (unsigned int)(occurrence - 1);
    utarray_push_back(*selected, &idx);
    return TRUE;
}

static bool edc_edit_file_select_by_line_hint(const char *content, size_t content_len,
                                                    unsigned int count, UT_array *line_nums,
                                                    UT_array *positions, size_t old_len,
                                                    int64_t line_hint, UT_array **selected,
                                                    EdcError **error)
{
    int64_t best_dist = G_MAXINT64;
    unsigned int best_idx = 0;
    bool ambiguous = FALSE;

    for (unsigned int i = 0; i < count; i++)
    {
        int64_t ln = *(int64_t*)utarray_eltptr(line_nums, (unsigned)i);
        int64_t dist = ln > line_hint ? ln - line_hint : line_hint - ln;
        if (dist < best_dist)
        {
            best_dist = dist;
            best_idx = i;
            ambiguous = FALSE;
        }
        else if (dist == best_dist)
            ambiguous = TRUE;
    }

    if (ambiguous)
    {
        sds msg = sdsempty();
        edc_edit_file_format_match_list(content, content_len, positions, line_nums,
                                         old_len, count, 5, &msg);
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: line_hint %ld is ambiguous; old_text appears %u times.%s",
                    (long)line_hint, count, msg);
        sdsfree(msg);
        return FALSE;
    }

    utarray_push_back(*selected, &best_idx);
    return TRUE;
}

static bool edc_edit_file_select_matches(const char *content, size_t content_len,
                                              unsigned int count, UT_array *line_nums,
                                              UT_array *positions, size_t old_len,
                                              bool replace_all, int64_t occurrence,
                                              int64_t line_hint, UT_array **selected, EdcError **error)
{
    utarray_new(*selected, &ut_uint_icd);

    if ((replace_all ? 1 : 0) + (occurrence > 0 ? 1 : 0) + (line_hint > 0 ? 1 : 0) > 1)
    {
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: occurrence, line_hint, and replace_all are mutually exclusive.");
        return FALSE;
    }

    if (count == 0)
    {
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: internal error — no matches to select from.");
        return FALSE;
    }

    if (replace_all)
        return edc_edit_file_select_all(count, selected, error);

    if (occurrence > 0)
        return edc_edit_file_select_by_occurrence(count, occurrence, selected, error);

    if (line_hint > 0)
        return edc_edit_file_select_by_line_hint(content, content_len, count, line_nums,
                                                  positions, old_len, line_hint,
                                                  selected, error);

    if (count > 1)
    {
        sds msg = sdsempty();
        edc_edit_file_format_match_list(content, content_len, positions, line_nums,
                                         old_len, count, 5, &msg);
        edc_error_set(error, EDC_ERR_EXEC,
                    "Warning: old_text appears %u times. "
                    "Provide more context, set occurrence, or set replace_all=true.%s",
                    count, msg);
        sdsfree(msg);
        return FALSE;
    }

    {
        unsigned int zero = 0;
        utarray_push_back(*selected, &zero);
    }
    return TRUE;
}

static void edc_edit_file_apply_replacements(const char *content, size_t content_len,
                                              const char *old_text, size_t old_len,
                                              const char *new_text, UT_array *positions,
                                              UT_array *selected, sds *out_content)
{
    size_t new_len = strlen(new_text);
    *out_content = sdsempty();
    size_t prev_end = 0;

    for (unsigned int si = 0; si < utarray_len(selected); si++)
    {
        unsigned int idx = *(unsigned int*)utarray_eltptr(selected, si);
        size_t start = *(size_t*)utarray_eltptr(positions, (unsigned)idx);
        size_t end = start + old_len;

        *out_content = sdscatlen(*out_content, content + prev_end, (ssize_t)(start - prev_end));
        *out_content = sdscat(*out_content, new_text);
        prev_end = end;

        if (new_len == 0 && end < content_len
            && content[end - 1] != '\n' && content[end] == '\n')
            prev_end++;
    }
    *out_content = sdscatlen(*out_content, content + prev_end, (ssize_t)(content_len - prev_end));
}

static sds edc_tool_exec_edit_file(struct edc_config *conf, cJSON *args, EdcError **error)
{
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    const char *old_text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "old_text"));
    const char *new_text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "new_text"));

    if (!path || !old_text || !new_text)
    {
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: missing required parameter 'path', 'old_text', or 'new_text'.");
        return NULL;
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
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: occurrence, line_hint, and expected_replacements must be non-negative.");
        return NULL;
    }

    sds safe_path = edc_sandbox_verify_path(conf, path, TRUE, error);
    if (!safe_path)
        return NULL;

    sds created = edc_edit_file_try_create(path, safe_path, old_text, new_text, error);
    if (created || *error)
    {
        sdsfree(safe_path);
        return created;
    }

    char *content = NULL;
    UT_array *positions = NULL;
    UT_array *line_nums = NULL;
    UT_array *selected = NULL;
    sds result = NULL;

    GError *gerror = NULL;
    size_t content_len;
    g_file_get_contents(safe_path, &content, &content_len, &gerror);
    if (gerror)
    {
        edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
        g_error_free(gerror);
        goto cleanup;
    }

    size_t old_len = strlen(old_text);
    unsigned int count;
    edc_edit_file_find_matches(content, old_text, old_len, &positions, &line_nums, &count);

    if (count == 0)
    {
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: old_text not found in %s", path);
        goto cleanup;
    }

    if (!edc_edit_file_select_matches(content, content_len, count, line_nums,
                                       positions, old_len, replace_all, occurrence,
                                       line_hint, &selected, error))
        goto cleanup;

    if (expected_replacements > 0 && utarray_len(selected) != (unsigned int)expected_replacements)
    {
        edc_error_set(error, EDC_ERR_EXEC,
                    "Error: expected %ld replacements but would make %u.",
                    (long)expected_replacements, (unsigned)utarray_len(selected));
        goto cleanup;
    }

    sds new_content;
    edc_edit_file_apply_replacements(content, content_len, old_text, old_len,
                                      new_text, positions, selected, &new_content);

    GError *gerror2 = NULL;
    g_file_set_contents(safe_path, new_content, -1, &gerror2);
    sdsfree(new_content);
    if (gerror2)
    {
        edc_error_set(error, EDC_ERR_FAILED, "%s", gerror2->message);
        g_error_free(gerror2);
    }
    else
    {
        result = sdscatprintf(sdsempty(), "Successfully edited %s", path);
    }

cleanup:
    free(content);
    sdsfree(safe_path);
    if (positions)
        utarray_free(positions);
    if (line_nums)
        utarray_free(line_nums);
    if (selected)
        utarray_free(selected);
    return result;
}

static void edc_tool_exec_edit_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                          void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{
    EdcError *error = NULL;
    sds result = edc_tool_exec_edit_file(ctx->tb->conf, args, &error);
    callback(ctx, result, error);
}

static void edc_tool_create_edit_file(struct edc_toolbox *tb)
{
    const char *name = "edit_file";
    const char *desc =
        "Perform a small, exact replacement in one file by replacing old_text with new_text. Use this for narrow text "
        "substitutions with old_text copied from read_file. For multi-file, structural, or generated code edits, "
        "prefer apply_patch. If old_text matches multiple times, provide more context or set occurrence, line_hint, "
        "replace_all, and expected_replacements. Shows closest-match diagnostics on failure.";

    struct edc_tool_param *path = edc_tool_param_new("path", edc_param_str_type("The file path to edit", NULL), TRUE);
    struct edc_tool_param *old_text =
        edc_tool_param_new("old_text", edc_param_str_type("The text to find and replace", NULL), TRUE);
    struct edc_tool_param *new_text =
        edc_tool_param_new("new_text", edc_param_str_type("The text to replace with", NULL), TRUE);
    struct edc_tool_param *replace_all =
        edc_tool_param_new("replace_all", edc_param_bool_type("Replace all occurrences (default false)"), FALSE);

    struct edc_tool_param *occurrence = edc_tool_param_new(
        "occurrence",
        edc_param_int_type("Optional 1-based occurrence to replace when old_text appears multiple times.", 1,
                           G_MAXINT64),
        FALSE);

    struct edc_tool_param *line_hint = edc_tool_param_new(
        "line_hint", edc_param_int_type("Optional 1-based line hint used to choose the nearest match.", 1, G_MAXINT64),
        FALSE);

    struct edc_tool_param *expected_replacements = edc_tool_param_new(
        "expected_replacements",
        edc_param_int_type("Optional guard for the number of replacements that must be made.", 1, G_MAXINT64), FALSE);

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
