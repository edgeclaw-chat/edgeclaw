#include "edc_sandbox.h"
#include "edc_toolbox.h"
#include "edc_tools.h"

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
    params = g_slist_append(params, edc_tool_param_new("path", edc_param_str_type("The file path to read", NULL), TRUE));
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

void edc_tools_fs_register(struct edc_toolbox *tb)
{
    edc_tool_create_read_file(tb);
    edc_tool_create_write_file(tb);
    edc_tool_create_list_dir(tb);
}
