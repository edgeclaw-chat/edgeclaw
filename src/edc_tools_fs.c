#include "edc_sandbox.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include <edc_param_types.h>
#include <string.h>

static GString *edc_tool_exec_read_file(struct edc_config *conf, cJSON *args, GError **error)
{
    const gchar *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));

    gchar *safe_path = edc_sandbox_verify_path(conf, path, FALSE, error);
    if (!safe_path)
    {
        return NULL;
    }

    gchar *content;
    gsize len = 0;
    *error = NULL;
    g_file_get_contents(safe_path, &content, &len, error);
    g_free(safe_path);
    if (*error)
    {
        return NULL;
    }

    return g_string_new_take(content);
}

static GString *edc_tool_exec_write_file(struct edc_config *conf, cJSON *args, GError **error)
{
    const gchar *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    const gchar *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "content"));

    gchar *safe_path = edc_sandbox_verify_path(conf, path, TRUE, error);
    if (!safe_path)
    {
        return NULL;
    }

    *error = NULL;
    g_file_set_contents(safe_path, content, -1, error);
    g_free(safe_path);
    if (*error)
    {
        return NULL;
    }

    GString *result = g_string_new("");
    g_string_append_printf(result, "Successfully wrote to file %s", path);
    return result;
}

static GString *edc_tool_exec_list_dir(struct edc_config *conf, cJSON *args, GError **error)
{
    const gchar *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));

    gchar *safe_path = edc_sandbox_verify_path(conf, path, FALSE, error);
    if (!safe_path)
    {
        return NULL;
    }

    GDir *dir = g_dir_open(safe_path, 0, error);
    g_free(safe_path);
    if (*error)
    {
        return NULL;
    }

    GString *result = g_string_new("");
    const gchar *name;
    while ((name = g_dir_read_name(dir)))
    {
        gchar *full_path = g_build_filename(path, name, NULL);
        gboolean is_dir = g_file_test(full_path, G_FILE_TEST_IS_DIR);
        g_free(full_path);

        const gchar *prefix = is_dir ? "[DIR]  " : "[FILE] ";
        g_string_append_printf(result, "%s%s\n", prefix, name);
    }

    g_dir_close(dir);
    return result;
}

static void edc_tool_exec_read_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                          void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    GError *error = NULL;
    GString *result = edc_tool_exec_read_file(ctx->tb->conf, args, &error);
    callback(ctx, result, error);
}

static void edc_tool_create_read_file(struct edc_toolbox *tb)
{
    const gchar *name = "read_file";
    const gchar *desc = "Read the contents of a file at the given path.";
    GSList *params = NULL;
    params =
        g_slist_append(params, edc_tool_param_new("path", edc_param_str_type("The file path to read", NULL), TRUE));
    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_read_file_async));
}

static void edc_tool_exec_write_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                           void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    GError *error = NULL;
    GString *result = edc_tool_exec_write_file(ctx->tb->conf, args, &error);
    callback(ctx, result, error);
}

static void edc_tool_create_write_file(struct edc_toolbox *tb)
{
    const gchar *name = "write_file";
    const gchar *desc = "Write the contents of a file at the given path.";

    GSList *params = NULL;
    params =
        g_slist_append(params, edc_tool_param_new("path", edc_param_str_type("The file path to write", NULL), TRUE));
    params =
        g_slist_append(params, edc_tool_param_new("content", edc_param_str_type("The content to write", NULL), TRUE));

    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_write_file_async));
}

static void edc_tool_exec_list_dir_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                         void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    GError *error = NULL;
    GString *result = edc_tool_exec_list_dir(ctx->tb->conf, args, &error);
    callback(ctx, result, error);
}

static void edc_tool_create_list_dir(struct edc_toolbox *tb)
{
    const gchar *name = "list_dir";
    const gchar *desc = "List the contents of a directory at the given path.";

    GSList *params = NULL;
    params = g_slist_append(params,
                            edc_tool_param_new("path", edc_param_str_type("The directory path to list", NULL), TRUE));
    edc_toolbox_register(tb, edc_tool_new(name, desc, params, edc_tool_exec_list_dir_async));
}

static GString *edc_edit_file_try_create(const gchar *path, gchar *safe_path,
                                            const gchar *old_text, const gchar *new_text,
                                            GError **error)
{
    if (!g_file_test(safe_path, G_FILE_TEST_EXISTS))
    {
        if (old_text && old_text[0] == '\0')
        {
            gchar *parent = g_path_get_dirname(safe_path);
            g_mkdir_with_parents(parent, 0755);
            g_free(parent);
            *error = NULL;
            g_file_set_contents(safe_path, new_text, -1, error);
            return *error ? NULL : g_string_new_take(g_strdup_printf("Successfully created %s", path));
        }
        g_set_error(error, BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                    "Error: File not found: %s", path);
        return NULL;
    }

    if (old_text && old_text[0] == '\0')
    {
        gchar *content;
        *error = NULL;
        g_file_get_contents(safe_path, &content, NULL, error);
        if (*error)
            return NULL;
        g_strstrip(content);
        if (content[0] != '\0')
        {
            g_free(content);
            g_set_error(error, BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                        "Error: Cannot create file — %s already exists and is not empty.", path);
            return NULL;
        }
        g_free(content);
        *error = NULL;
        g_file_set_contents(safe_path, new_text, -1, error);
        return *error ? NULL : g_string_new_take(g_strdup_printf("Successfully edited %s", path));
    }

    return NULL;
}

static void edc_edit_file_find_matches(const gchar *content, const gchar *old_text, gsize old_len,
                                        GArray **positions, GArray **line_nums, guint *out_count)
{
    *positions = g_array_new(FALSE, FALSE, sizeof(gsize));
    *line_nums = g_array_new(FALSE, FALSE, sizeof(gint64));

    const gchar *scan = content;
    gint64 current_line = 1;
    const gchar *nl_scan = content;

    while ((scan = strstr(scan, old_text)) != NULL)
    {
        gsize pos = (gsize)(scan - content);
        while (nl_scan < scan)
        {
            if (*nl_scan == '\n')
                current_line++;
            nl_scan++;
        }
        g_array_append_val(*positions, pos);
        g_array_append_val(*line_nums, current_line);
        scan += old_len;
    }
    *out_count = (*positions)->len;
}

static gboolean edc_edit_file_select_matches(guint count, GArray *line_nums,
                                              gboolean replace_all, gint64 occurrence,
                                              gint64 line_hint, GArray **selected, GError **error)
{
    *selected = g_array_new(FALSE, FALSE, sizeof(guint));

    if (replace_all && occurrence > 0)
        goto mutual_exclusive;
    if (replace_all && line_hint > 0)
        goto mutual_exclusive;
    if (occurrence > 0 && line_hint > 0)
        goto mutual_exclusive;

    if (replace_all)
    {
        for (guint i = 0; i < count; i++)
        {
            guint idx = i;
            g_array_append_val(*selected, idx);
        }
        return TRUE;
    }

    if (occurrence > 0)
    {
        if ((guint64)occurrence > count)
        {
            g_set_error(error, BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                        "Error: occurrence %ld out of range; old_text appears %u times.",
                        (long)occurrence, count);
            return FALSE;
        }
        guint idx = (guint)(occurrence - 1);
        g_array_append_val(*selected, idx);
        return TRUE;
    }

    if (line_hint > 0)
    {
        gint64 best_dist = G_MAXINT64;
        guint best_idx = 0;
        gboolean ambiguous = FALSE;
        for (guint i = 0; i < count; i++)
        {
            gint64 ln = g_array_index(line_nums, gint64, i);
            gint64 dist = ln > line_hint ? ln - line_hint : line_hint - ln;
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
            g_set_error(error, BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                        "Error: line_hint %ld is ambiguous; old_text appears %u times.",
                        (long)line_hint, count);
            return FALSE;
        }
        g_array_append_val(*selected, best_idx);
        return TRUE;
    }

    if (count > 1)
    {
        GString *loc_msg = g_string_new(NULL);
        guint preview = MIN(count, 3);
        for (guint i = 0; i < preview; i++)
        {
            if (i > 0)
                g_string_append(loc_msg, ", ");
            g_string_append_printf(loc_msg, "line %ld",
                                   (long)g_array_index(line_nums, gint64, i));
        }
        if (count > 3)
            g_string_append(loc_msg, ", ...");
        g_set_error(error, BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                    "Warning: old_text appears %u times at %s. "
                    "Provide more context, set occurrence, or set replace_all=true.",
                    count, loc_msg->str);
        g_string_free(loc_msg, TRUE);
        return FALSE;
    }

    {
        guint zero = 0;
        g_array_append_val(*selected, zero);
    }
    return TRUE;

mutual_exclusive:
    g_set_error(error, BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                "Error: occurrence, line_hint, and replace_all are mutually exclusive.");
    return FALSE;
}

static void edc_edit_file_apply_replacements(const gchar *content, gsize content_len,
                                              const gchar *old_text, gsize old_len,
                                              const gchar *new_text, GArray *positions,
                                              GArray *selected, GString **out_content)
{
    gsize new_len = strlen(new_text);
    *out_content = g_string_sized_new(content_len + selected->len * new_len);
    gsize prev_end = 0;

    for (guint si = 0; si < selected->len; si++)
    {
        guint idx = g_array_index(selected, guint, si);
        gsize start = g_array_index(positions, gsize, idx);
        gsize end = start + old_len;

        g_string_append_len(*out_content, content + prev_end, (gssize)(start - prev_end));
        g_string_append(*out_content, new_text);
        prev_end = end;

        if (new_len == 0 && end < content_len
            && content[end - 1] != '\n' && content[end] == '\n')
            prev_end++;
    }
    g_string_append_len(*out_content, content + prev_end, (gssize)(content_len - prev_end));
}

static GString *edc_tool_exec_edit_file(struct edc_config *conf, cJSON *args, GError **error)
{
    const gchar *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "path"));
    const gchar *old_text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "old_text"));
    const gchar *new_text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "new_text"));

    gboolean replace_all = cJSON_IsTrue(cJSON_GetObjectItem(args, "replace_all"));

    cJSON *j;
    gint64 occurrence = 0;
    j = cJSON_GetObjectItem(args, "occurrence");
    if (j && cJSON_IsNumber(j))
        occurrence = (gint64)j->valuedouble;

    gint64 line_hint = 0;
    j = cJSON_GetObjectItem(args, "line_hint");
    if (j && cJSON_IsNumber(j))
        line_hint = (gint64)j->valuedouble;

    gint64 expected_replacements = 0;
    j = cJSON_GetObjectItem(args, "expected_replacements");
    if (j && cJSON_IsNumber(j))
        expected_replacements = (gint64)j->valuedouble;

    gchar *safe_path = edc_sandbox_verify_path(conf, path, TRUE, error);
    if (!safe_path)
        return NULL;

    GString *created = edc_edit_file_try_create(path, safe_path, old_text, new_text, error);
    if (created || *error)
    {
        g_free(safe_path);
        return created;
    }

    gchar *content = NULL;
    GArray *positions = NULL;
    GArray *line_nums = NULL;
    GArray *selected = NULL;
    GString *result = NULL;

    *error = NULL;
    gsize content_len;
    g_file_get_contents(safe_path, &content, &content_len, error);
    if (*error)
        goto cleanup;

    gsize old_len = strlen(old_text);
    guint count;
    edc_edit_file_find_matches(content, old_text, old_len, &positions, &line_nums, &count);

    if (count == 0)
    {
        g_set_error(error, BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                    "Error: old_text not found in %s", path);
        goto cleanup;
    }

    if (!edc_edit_file_select_matches(count, line_nums, replace_all, occurrence,
                                       line_hint, &selected, error))
        goto cleanup;

    if (expected_replacements > 0 && selected->len != (guint)expected_replacements)
    {
        g_set_error(error, BC_TOOL_ERROR, BC_TOOL_EXEC_ERROR,
                    "Error: expected %ld replacements but would make %u.",
                    (long)expected_replacements, selected->len);
        goto cleanup;
    }

    GString *new_content;
    edc_edit_file_apply_replacements(content, content_len, old_text, old_len,
                                      new_text, positions, selected, &new_content);

    *error = NULL;
    g_file_set_contents(safe_path, new_content->str, -1, error);
    g_string_free(new_content, TRUE);
    if (!*error)
        result = g_string_new_take(g_strdup_printf("Successfully edited %s", path));

cleanup:
    g_free(content);
    g_free(safe_path);
    if (positions)
        g_array_free(positions, TRUE);
    if (line_nums)
        g_array_free(line_nums, TRUE);
    if (selected)
        g_array_free(selected, TRUE);
    return result;
}

static void edc_tool_exec_edit_file_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                          void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    GError *error = NULL;
    GString *result = edc_tool_exec_edit_file(ctx->tb->conf, args, &error);
    callback(ctx, result, error);
}

static void edc_tool_create_edit_file(struct edc_toolbox *tb)
{
    const gchar *name = "edit_file";
    const gchar *desc =
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
