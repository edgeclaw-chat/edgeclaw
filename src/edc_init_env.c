#include "edc_init_env.h"
#include "edc_secret.h"
#include "cJSON.h"
#include "linenoise.h"
#include "log.h"
#include <dirent.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define MAX_FIELD_LEN 512

/* Channel type options */
static const char *channel_types[] = {"cli", "telegram", "email", "feishu", "qq"};
#define CHANNEL_TYPE_COUNT 5

/* --- Helper functions --- */

/* Read a line with linenoise, applying a default if the user enters nothing.
 * Returns a malloc'd string (caller must free), or NULL on EOF/Ctrl-C. */
static char *prompt_with_default(const char *label, const char *default_val)
{
    char prompt[1024];
    if (default_val && default_val[0] != '\0')
    {
        snprintf(prompt, sizeof(prompt), "%s [%s]: ", label, default_val);
    }
    else
    {
        snprintf(prompt, sizeof(prompt), "%s: ", label);
    }

    char *line = linenoise(prompt);
    if (line == NULL)
    {
        return NULL; /* EOF or Ctrl-C */
    }

    /* Strip trailing whitespace (\n, \r) that linenoise may leave in fallback mode */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
    {
        line[--len] = '\0';
    }

    /* If user entered nothing, use default */
    if (line[0] == '\0' && default_val && default_val[0] != '\0')
    {
        free(line);
        return strdup(default_val);
    }

    return line;
}

/* Read a password (no echo). If default_val is non-empty, shows [***unchanged***]
 * and returns a copy of it when the user enters nothing.
 * Returns malloc'd string, or NULL on error. */
static char *prompt_password(const char *label, const char *default_val)
{
    char prompt[1024];
    if (default_val && default_val[0] != '\0')
    {
        snprintf(prompt, sizeof(prompt), "%s [***unchanged***]: ", label);
    }
    else
    {
        snprintf(prompt, sizeof(prompt), "%s: ", label);
    }
    fprintf(stdout, "%s", prompt);
    fflush(stdout);

    struct termios old_term, new_term;
    if (tcgetattr(STDIN_FILENO, &old_term) != 0)
    {
        return NULL;
    }

    new_term = old_term;
    new_term.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    char buf[MAX_FIELD_LEN];
    char *result = NULL;

    if (fgets(buf, sizeof(buf), stdin) != NULL)
    {
        /* Strip newline */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
        {
            buf[len - 1] = '\0';
        }
        result = strdup(buf);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    fprintf(stdout, "\n");
    fflush(stdout);

    /* If user entered nothing and a default exists, keep the old value */
    if (result && result[0] == '\0' && default_val && default_val[0] != '\0')
    {
        free(result);
        return strdup(default_val);
    }

    return result;
}

/* Prompt for an integer with validation. Returns the value, or -1 on cancel. */
static int prompt_int(const char *label, int default_val)
{
    char default_str[32];
    snprintf(default_str, sizeof(default_str), "%d", default_val);

    while (1)
    {
        char *line = prompt_with_default(label, default_str);
        if (line == NULL)
        {
            return -1;
        }

        int val = 0;
        if (sscanf(line, "%d", &val) == 1)
        {
            free(line);
            return val;
        }

        fprintf(stderr, "  Invalid number, please try again.\n");
        free(line);
    }
}

/* Prompt for channel type selection. Returns index, or -1 on cancel. */
static int prompt_channel_type(int default_idx)
{
    if (default_idx < 0 || default_idx >= CHANNEL_TYPE_COUNT)
        default_idx = 0;

    printf("\n--- Channel Type ---\n");
    for (int i = 0; i < CHANNEL_TYPE_COUNT; i++)
    {
        printf("  %d) %s\n", i + 1, channel_types[i]);
    }

    char default_str[32];
    snprintf(default_str, sizeof(default_str), "%d", default_idx + 1);

    while (1)
    {
        char *line = prompt_with_default("Select channel", default_str);
        if (line == NULL)
        {
            return -1;
        }

        int choice = 0;
        if (sscanf(line, "%d", &choice) == 1 && choice >= 1 && choice <= CHANNEL_TYPE_COUNT)
        {
            free(line);
            return choice - 1;
        }

        free(line);
        fprintf(stderr, "  Invalid choice, enter 1-%d.\n", CHANNEL_TYPE_COUNT);
    }
}

/* Parse existing config.json into a edc_init_env_result.
 * Returns a newly allocated struct, or NULL if the file doesn't exist or can't be parsed. */
static struct edc_init_env_result *load_existing_config(const gchar *conf_file)
{
    if (!g_file_test(conf_file, G_FILE_TEST_IS_REGULAR))
        return NULL;

    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(conf_file, &contents, &length, &error))
    {
        if (error)
            g_error_free(error);
        return NULL;
    }

    cJSON *json = cJSON_Parse(contents);
    g_free(contents);
    if (!json)
        return NULL;

    struct edc_init_env_result *result = g_malloc0(sizeof(*result));

    cJSON *item;
    item = cJSON_GetObjectItem(json, "api_key");
    if (item && cJSON_IsString(item))
    {
        gchar *decrypted = edc_secret_decrypt(item->valuestring);
        if (decrypted)
        {
            strncpy(result->api_key, decrypted, sizeof(result->api_key) - 1);
            g_free(decrypted);
        }
    }

    item = cJSON_GetObjectItem(json, "api_url");
    if (item && cJSON_IsString(item))
        strncpy(result->api_url, item->valuestring, sizeof(result->api_url) - 1);

    item = cJSON_GetObjectItem(json, "model");
    if (item && cJSON_IsString(item))
        strncpy(result->model, item->valuestring, sizeof(result->model) - 1);

    item = cJSON_GetObjectItem(json, "max_iter");
    if (item && cJSON_IsNumber(item))
        result->max_iter = item->valueint;

    item = cJSON_GetObjectItem(json, "load_n_histories");
    if (item && cJSON_IsNumber(item))
        result->load_n_histories = item->valueint;

    item = cJSON_GetObjectItem(json, "memory_compress_threshold");
    if (item && cJSON_IsNumber(item))
        result->memory_compress_threshold = item->valueint;

    item = cJSON_GetObjectItem(json, "tool_max_output_size");
    if (item && cJSON_IsNumber(item))
        result->tool_max_output_size = item->valueint;

    item = cJSON_GetObjectItem(json, "sandbox");
    if (item && cJSON_IsObject(item))
    {
        cJSON *rw = cJSON_GetObjectItem(item, "rw_paths");
        if (rw && cJSON_IsArray(rw))
        {
            int size = cJSON_GetArraySize(rw);
            if (size > 0)
            {
                cJSON *first = cJSON_GetArrayItem(rw, 0);
                if (first && cJSON_IsString(first))
                    strncpy(result->sandbox_root, first->valuestring, sizeof(result->sandbox_root) - 1);

                if (size > 1)
                {
                    GString *joined = g_string_new(NULL);
                    for (int i = 1; i < size; i++)
                    {
                        cJSON *elem = cJSON_GetArrayItem(rw, i);
                        if (elem && cJSON_IsString(elem))
                        {
                            if (joined->len > 0)
                                g_string_append(joined, ", ");
                            g_string_append(joined, elem->valuestring);
                        }
                    }
                    strncpy(result->sandbox_rw_paths, joined->str, sizeof(result->sandbox_rw_paths) - 1);
                    g_string_free(joined, TRUE);
                }
            }
        }

        cJSON *ro = cJSON_GetObjectItem(item, "ro_paths");
        if (ro && cJSON_IsArray(ro))
        {
            int size = cJSON_GetArraySize(ro);
            if (size > 0)
            {
                GString *joined = g_string_new(NULL);
                for (int i = 0; i < size; i++)
                {
                    cJSON *elem = cJSON_GetArrayItem(ro, i);
                    if (elem && cJSON_IsString(elem))
                    {
                        if (joined->len > 0)
                            g_string_append(joined, ", ");
                        g_string_append(joined, elem->valuestring);
                    }
                }
                strncpy(result->sandbox_ro_paths, joined->str, sizeof(result->sandbox_ro_paths) - 1);
                g_string_free(joined, TRUE);
            }
        }
    }

    cJSON *channel = cJSON_GetObjectItem(json, "channel");
    if (channel)
    {
        item = cJSON_GetObjectItem(channel, "type");
        if (item && cJSON_IsString(item))
            strncpy(result->channel_type, item->valuestring, sizeof(result->channel_type) - 1);

        const char *ctype = result->channel_type;

        if (strcmp(ctype, "email") == 0)
        {
            item = cJSON_GetObjectItem(channel, "imap_host");
            if (item && cJSON_IsString(item))
                strncpy(result->email.imap_host, item->valuestring, sizeof(result->email.imap_host) - 1);

            item = cJSON_GetObjectItem(channel, "smtp_host");
            if (item && cJSON_IsString(item))
                strncpy(result->email.smtp_host, item->valuestring, sizeof(result->email.smtp_host) - 1);

            item = cJSON_GetObjectItem(channel, "user");
            if (item && cJSON_IsString(item))
                strncpy(result->email.user, item->valuestring, sizeof(result->email.user) - 1);

            item = cJSON_GetObjectItem(channel, "passwd");
            if (item && cJSON_IsString(item))
            {
                gchar *decrypted = edc_secret_decrypt(item->valuestring);
                if (decrypted)
                {
                    strncpy(result->email.passwd, decrypted, sizeof(result->email.passwd) - 1);
                    g_free(decrypted);
                }
            }

            item = cJSON_GetObjectItem(channel, "target");
            if (item && cJSON_IsString(item))
                strncpy(result->email.target, item->valuestring, sizeof(result->email.target) - 1);
        }
        else if (strcmp(ctype, "telegram") == 0)
        {
            item = cJSON_GetObjectItem(channel, "bot_key");
            if (item && cJSON_IsString(item))
            {
                gchar *decrypted = edc_secret_decrypt(item->valuestring);
                if (decrypted)
                {
                    strncpy(result->telegram.bot_key, decrypted, sizeof(result->telegram.bot_key) - 1);
                    g_free(decrypted);
                }
            }

            item = cJSON_GetObjectItem(channel, "user_id");
            if (item && cJSON_IsNumber(item))
                result->telegram.user_id = (gint64)item->valuedouble;
        }
        else if (strcmp(ctype, "feishu") == 0)
        {
            item = cJSON_GetObjectItem(channel, "app_id");
            if (item && cJSON_IsString(item))
                strncpy(result->feishu.app_id, item->valuestring, sizeof(result->feishu.app_id) - 1);

            item = cJSON_GetObjectItem(channel, "app_secret");
            if (item && cJSON_IsString(item))
            {
                gchar *decrypted = edc_secret_decrypt(item->valuestring);
                if (decrypted)
                {
                    strncpy(result->feishu.app_secret, decrypted, sizeof(result->feishu.app_secret) - 1);
                    g_free(decrypted);
                }
            }
        }
        else if (strcmp(ctype, "qq") == 0)
        {
            item = cJSON_GetObjectItem(channel, "app_id");
            if (item && cJSON_IsString(item))
                strncpy(result->qq.app_id, item->valuestring, sizeof(result->qq.app_id) - 1);

            item = cJSON_GetObjectItem(channel, "app_secret");
            if (item && cJSON_IsString(item))
            {
                gchar *decrypted = edc_secret_decrypt(item->valuestring);
                if (decrypted)
                {
                    strncpy(result->qq.app_secret, decrypted, sizeof(result->qq.app_secret) - 1);
                    g_free(decrypted);
                }
            }
        }
    }

    cJSON_Delete(json);
    return result;
}

static int edc_init_env_get_configure(struct edc_init_env_result *result, const struct edc_init_env_result *defaults,
                                     const gchar *edc_home)
{
    if (!result)
        return -1;

    memset(result, 0, sizeof(*result));

    printf("\n========== EdgeClaw Configuration Setup ==========\n");

    /* --- LLM API Configuration --- */
    printf("\n--- LLM API Configuration ---\n");

    /* API URL */
    const char *def_api_url = (defaults && defaults->api_url[0])
                                  ? defaults->api_url
                                  : "https://open.bigmodel.cn/api/coding/paas/v4/chat/completions";
    char *val = prompt_with_default("API URL", def_api_url);
    if (!val)
        return -1;
    strncpy(result->api_url, val, sizeof(result->api_url) - 1);
    free(val);

    /* Model */
    const char *def_model = (defaults && defaults->model[0]) ? defaults->model : "glm-5";
    val = prompt_with_default("Model", def_model);
    if (!val)
        return -1;
    strncpy(result->model, val, sizeof(result->model) - 1);
    free(val);

    /* API Key (password) */
    val = prompt_password("API Key", (defaults && defaults->api_key[0]) ? defaults->api_key : NULL);
    if (!val)
        return -1;
    strncpy(result->api_key, val, sizeof(result->api_key) - 1);
    free(val);

    /* Max Iterations */
    int ival = prompt_int("Max Iterations", defaults && defaults->max_iter > 0 ? defaults->max_iter : 20);
    if (ival < 0)
        return -1;
    result->max_iter = ival;

    /* Load N Histories */
    ival = prompt_int("Load N Histories", defaults ? defaults->load_n_histories : 10);
    if (ival < 0)
        return -1;
    result->load_n_histories = ival;

    /* Memory Compress Threshold */
    ival = prompt_int("Memory Compress Threshold",
                      defaults && defaults->memory_compress_threshold > 0 ? defaults->memory_compress_threshold : 100);
    if (ival < 0)
        return -1;
    result->memory_compress_threshold = ival;

    /* Tool Max Output Size */
    ival = prompt_int("Tool Max Output Size",
                      defaults && defaults->tool_max_output_size > 0 ? defaults->tool_max_output_size : 10000);
    if (ival < 0)
        return -1;
    result->tool_max_output_size = ival;

    /* Sandbox Configuration */
    printf("\n--- Sandbox Configuration ---\n");

    {
        gchar default_sandbox[512];
        snprintf(default_sandbox, sizeof(default_sandbox), "%s/sandbox", edc_home);
        const char *def = (defaults && defaults->sandbox_root[0]) ? defaults->sandbox_root : default_sandbox;
        val = prompt_with_default("Sandbox Root (working directory, writable)", def);
        if (!val)
            return -1;
        strncpy(result->sandbox_root, val, sizeof(result->sandbox_root) - 1);
        free(val);
    }

    {
        gchar *def_rw_str =
            g_strdup_printf("%s/workspace/, /tmp, /dev", edc_home);
        const char *def_rw = (defaults && defaults->sandbox_rw_paths[0]) ? defaults->sandbox_rw_paths : def_rw_str;
        val = prompt_with_default("  Additional writable paths (comma-separated)", def_rw);
        g_free(def_rw_str);
        if (!val)
            return -1;
        strncpy(result->sandbox_rw_paths, val, sizeof(result->sandbox_rw_paths) - 1);
        free(val);
    }

    {
        gchar *def_ro_str =
            g_strdup_printf("%s/workspace/ssl, /usr, /lib, /lib32, /lib64, /libx32, /bin, /sbin, /etc, /proc",
                            edc_home);
        const char *def_ro = (defaults && defaults->sandbox_ro_paths[0])
                                 ? defaults->sandbox_ro_paths
                                 : def_ro_str;
        val = prompt_with_default("  Read-only paths (comma-separated)", def_ro);
        g_free(def_ro_str);
        if (!val)
            return -1;
        strncpy(result->sandbox_ro_paths, val, sizeof(result->sandbox_ro_paths) - 1);
        free(val);
    }

    /* --- Channel Type --- */
    int default_channel = 0;
    if (defaults && defaults->channel_type[0])
    {
        for (int i = 0; i < CHANNEL_TYPE_COUNT; i++)
        {
            if (strcmp(defaults->channel_type, channel_types[i]) == 0)
            {
                default_channel = i;
                break;
            }
        }
    }
    int channel_idx = prompt_channel_type(default_channel);
    if (channel_idx < 0)
        return -1;
    strncpy(result->channel_type, channel_types[channel_idx], sizeof(result->channel_type) - 1);

    /* --- Channel-specific settings --- */
    const char *ctype = result->channel_type;

    if (strcmp(ctype, "email") == 0)
    {
        printf("\n--- Email Settings ---\n");
        gboolean same_chan = (defaults && strcmp(defaults->channel_type, "email") == 0);

        const char *def = (same_chan && defaults->email.imap_host[0]) ? defaults->email.imap_host : "imap.qq.com";
        val = prompt_with_default("IMAP Host", def);
        if (!val)
            return -1;
        strncpy(result->email.imap_host, val, sizeof(result->email.imap_host) - 1);
        free(val);

        def = (same_chan && defaults->email.smtp_host[0]) ? defaults->email.smtp_host : "smtp.qq.com";
        val = prompt_with_default("SMTP Host", def);
        if (!val)
            return -1;
        strncpy(result->email.smtp_host, val, sizeof(result->email.smtp_host) - 1);
        free(val);

        def = (same_chan && defaults->email.user[0]) ? defaults->email.user : "";
        val = prompt_with_default("Email User", def);
        if (!val)
            return -1;
        strncpy(result->email.user, val, sizeof(result->email.user) - 1);
        free(val);

        val =
            prompt_password("Email Password", (same_chan && defaults->email.passwd[0]) ? defaults->email.passwd : NULL);
        if (!val)
            return -1;
        strncpy(result->email.passwd, val, sizeof(result->email.passwd) - 1);
        free(val);

        def = (same_chan && defaults->email.target[0]) ? defaults->email.target : "";
        val = prompt_with_default("Target Email", def);
        if (!val)
            return -1;
        strncpy(result->email.target, val, sizeof(result->email.target) - 1);
        free(val);
    }
    else if (strcmp(ctype, "telegram") == 0)
    {
        printf("\n--- Telegram Settings ---\n");
        gboolean same_chan = (defaults && strcmp(defaults->channel_type, "telegram") == 0);

        val = prompt_password("Bot Key",
                              (same_chan && defaults->telegram.bot_key[0]) ? defaults->telegram.bot_key : NULL);
        if (!val)
            return -1;
        strncpy(result->telegram.bot_key, val, sizeof(result->telegram.bot_key) - 1);
        free(val);

        /* User ID is an integer */
        while (1)
        {
            char uid_buf[64] = "";
            const char *uid_def = uid_buf;
            if (same_chan && defaults->telegram.user_id != 0)
            {
                snprintf(uid_buf, sizeof(uid_buf), "%" G_GINT64_FORMAT, defaults->telegram.user_id);
            }
            val = prompt_with_default("User ID", uid_def);
            if (!val)
                return -1;
            gint64 uid = 0;
            if (sscanf(val, "%" G_GINT64_FORMAT, &uid) == 1 && uid != 0)
            {
                result->telegram.user_id = uid;
                free(val);
                break;
            }
            fprintf(stderr, "  Invalid number, please try again.\n");
            free(val);
        }
    }
    else if (strcmp(ctype, "feishu") == 0)
    {
        printf("\n--- Feishu Settings ---\n");
        gboolean same_chan = (defaults && strcmp(defaults->channel_type, "feishu") == 0);

        const char *def = (same_chan && defaults->feishu.app_id[0]) ? defaults->feishu.app_id : "";
        val = prompt_with_default("Feishu App ID", def);
        if (!val)
            return -1;
        strncpy(result->feishu.app_id, val, sizeof(result->feishu.app_id) - 1);
        free(val);

        val = prompt_password("Feishu App Secret",
                              (same_chan && defaults->feishu.app_secret[0]) ? defaults->feishu.app_secret : NULL);
        if (!val)
            return -1;
        strncpy(result->feishu.app_secret, val, sizeof(result->feishu.app_secret) - 1);
        free(val);
    }
    else if (strcmp(ctype, "qq") == 0)
    {
        printf("\n--- QQ Settings ---\n");
        gboolean same_chan = (defaults && strcmp(defaults->channel_type, "qq") == 0);

        const char *def = (same_chan && defaults->qq.app_id[0]) ? defaults->qq.app_id : "";
        val = prompt_with_default("QQ App ID", def);
        if (!val)
            return -1;
        strncpy(result->qq.app_id, val, sizeof(result->qq.app_id) - 1);
        free(val);

        val = prompt_password("QQ App Secret",
                              (same_chan && defaults->qq.app_secret[0]) ? defaults->qq.app_secret : NULL);
        if (!val)
            return -1;
        strncpy(result->qq.app_secret, val, sizeof(result->qq.app_secret) - 1);
        free(val);
    }

    printf("\n===================================================\n");
    return 0;
}

/* Copy directory recursively */
static gboolean copy_directory_recursive(const gchar *src_dir, const gchar *dest_dir)
{
    DIR *dir = opendir(src_dir);
    if (!dir)
    {
        log_error("Failed to open source directory %s", src_dir);
        return FALSE;
    }

    if (g_mkdir_with_parents(dest_dir, 0755) != 0)
    {
        log_error("Failed to create destination directory %s", dest_dir);
        closedir(dir);
        return FALSE;
    }

    struct dirent *entry;
    gboolean success = TRUE;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        gchar *src_path = g_build_filename(src_dir, entry->d_name, NULL);
        gchar *dest_path = g_build_filename(dest_dir, entry->d_name, NULL);

        struct stat st;
        if (lstat(src_path, &st) != 0)
        {
            log_error("Failed to stat %s", src_path);
            success = FALSE;
            g_free(src_path);
            g_free(dest_path);
            break;
        }

        if (S_ISDIR(st.st_mode))
        {
            if (!copy_directory_recursive(src_path, dest_path))
            {
                success = FALSE;
                g_free(src_path);
                g_free(dest_path);
                break;
            }
        }
        else
        {
            gchar *contents = NULL;
            gsize length = 0;
            GError *error = NULL;

            if (!g_file_get_contents(src_path, &contents, &length, &error))
            {
                log_error("Failed to read %s: %s", src_path, error ? error->message : "Unknown");
                if (error)
                    g_error_free(error);
                success = FALSE;
                g_free(src_path);
                g_free(dest_path);
                break;
            }

            if (!g_file_set_contents(dest_path, contents, length, &error))
            {
                log_error("Failed to write %s: %s", dest_path, error ? error->message : "Unknown");
                if (error)
                    g_error_free(error);
                g_free(contents);
                success = FALSE;
                g_free(src_path);
                g_free(dest_path);
                break;
            }

            g_free(contents);
        }

        g_free(src_path);
        g_free(dest_path);
    }

    closedir(dir);
    return success;
}

static gboolean edc_init_env_workspace(const gchar *dest_workspace)
{
    if (g_file_test(dest_workspace, G_FILE_TEST_IS_DIR))
    {
        return TRUE;
    }

    gchar *current_dir = g_get_current_dir();
    gchar *src_workspace = g_build_filename(current_dir, "workspace", NULL);
    g_free(current_dir);

    if (!g_file_test(src_workspace, G_FILE_TEST_IS_DIR))
    {
        log_error("Source workspace directory %s does not exist", src_workspace);
        g_free(src_workspace);
        return FALSE;
    }

    log_info("Copying workspace from %s to %s", src_workspace, dest_workspace);

    gboolean success = copy_directory_recursive(src_workspace, dest_workspace);
    g_free(src_workspace);

    if (!success)
    {
        log_error("Failed to copy workspace directory");
        return FALSE;
    }

    log_info("Workspace directory copied successfully");
    return TRUE;
}

static gchar **split_paths(const char *input)
{
    if (!input || !input[0])
        return NULL;

    gchar **parts = g_strsplit(input, ",", -1);
    if (!parts || !parts[0])
        return NULL;

    int count = 0;
    for (int i = 0; parts[i]; i++)
    {
        g_strstrip(parts[i]);
        if (parts[i][0])
            count++;
    }

    if (count == 0)
    {
        g_strfreev(parts);
        return NULL;
    }

    gchar **result = g_new0(gchar *, count + 1);
    int idx = 0;
    for (int i = 0; parts[i]; i++)
    {
        if (parts[i][0])
            result[idx++] = g_strdup(parts[i]);
    }
    g_strfreev(parts);
    return result;
}

static void build_sandbox_rw(cJSON *sandbox, const char *root, const char *rw_paths_str)
{
    cJSON *arr = cJSON_CreateArray();
    if (root && root[0])
        cJSON_AddItemToArray(arr, cJSON_CreateString(root));

    gchar **extra = split_paths(rw_paths_str);
    if (extra)
    {
        for (int i = 0; extra[i]; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(extra[i]));
        g_strfreev(extra);
    }

    cJSON_AddItemToObject(sandbox, "rw_paths", arr);
}

static gboolean edc_init_env_conf_file(const gchar *conf_file, const gchar *edc_home)
{
    gboolean is_create = !g_file_test(conf_file, G_FILE_TEST_IS_REGULAR);
    gchar *edc_home_dir = g_path_get_dirname(conf_file);

    if (!edc_secret_init(is_create, edc_home_dir))
    {
        log_error("Failed to initialize secret module");
        g_free(edc_home_dir);
        return FALSE;
    }
    g_free(edc_home_dir);

    struct edc_init_env_result *existing = load_existing_config(conf_file);

    struct edc_init_env_result env_result;
    memset(&env_result, 0, sizeof(env_result));

    if (edc_init_env_get_configure(&env_result, existing, edc_home) != 0)
    {
        log_error("Configuration cancelled or failed.");
        g_free(existing);
        return FALSE;
    }
    g_free(existing);

    /* Validate required fields */
    if (strlen(env_result.api_key) == 0)
    {
        log_error("API Key is required.");
        return FALSE;
    }
    if (strlen(env_result.api_url) == 0)
    {
        log_error("API URL is required.");
        return FALSE;
    }
    if (strlen(env_result.model) == 0)
    {
        log_error("Model is required.");
        return FALSE;
    }
    if (env_result.max_iter <= 0)
    {
        log_error("Max Iterations must be greater than 0.");
        return FALSE;
    }
    if (env_result.load_n_histories < 0)
    {
        log_error("Load N Histories must be >= 0.");
        return FALSE;
    }
    if (env_result.tool_max_output_size <= 0)
    {
        log_error("Tool Max Output Size must be greater than 0.");
        return FALSE;
    }

    /* Validate channel-specific fields */
    const char *ctype = env_result.channel_type;

    if (strcmp(ctype, "email") == 0)
    {
        if (strlen(env_result.email.imap_host) == 0)
        {
            log_error("IMAP Host is required for email channel.");
            return FALSE;
        }
        if (strlen(env_result.email.smtp_host) == 0)
        {
            log_error("SMTP Host is required for email channel.");
            return FALSE;
        }
        if (strlen(env_result.email.user) == 0)
        {
            log_error("Email User is required for email channel.");
            return FALSE;
        }
        if (strlen(env_result.email.passwd) == 0)
        {
            log_error("Email Password is required for email channel.");
            return FALSE;
        }
        if (strlen(env_result.email.target) == 0)
        {
            log_error("Target Email is required for email channel.");
            return FALSE;
        }
    }
    else if (strcmp(ctype, "telegram") == 0)
    {
        if (strlen(env_result.telegram.bot_key) == 0)
        {
            log_error("Bot Key is required for telegram channel.");
            return FALSE;
        }
        if (env_result.telegram.user_id == 0)
        {
            log_error("User ID is required for telegram channel.");
            return FALSE;
        }
    }
    else if (strcmp(ctype, "feishu") == 0)
    {
        if (strlen(env_result.feishu.app_id) == 0)
        {
            log_error("Feishu App ID is required for feishu channel.");
            return FALSE;
        }
        if (strlen(env_result.feishu.app_secret) == 0)
        {
            log_error("Feishu App Secret is required for feishu channel.");
            return FALSE;
        }
    }
    else if (strcmp(ctype, "qq") == 0)
    {
        if (strlen(env_result.qq.app_id) == 0)
        {
            log_error("QQ App ID is required for qq channel.");
            return FALSE;
        }
        if (strlen(env_result.qq.app_secret) == 0)
        {
            log_error("QQ App Secret is required for qq channel.");
            return FALSE;
        }
    }

    /* Build JSON config */
    cJSON *config = cJSON_CreateObject();

    gchar *encrypted_api_key = edc_secret_encrypt(env_result.api_key);
    cJSON_AddStringToObject(config, "api_key", encrypted_api_key ? encrypted_api_key : "");

    cJSON_AddStringToObject(config, "model", env_result.model);
    cJSON_AddStringToObject(config, "api_url", env_result.api_url);
    cJSON_AddNumberToObject(config, "max_iter", env_result.max_iter);
    cJSON_AddNumberToObject(config, "load_n_histories", env_result.load_n_histories);
    cJSON_AddNumberToObject(config, "memory_compress_threshold", env_result.memory_compress_threshold);
    cJSON_AddNumberToObject(config, "tool_max_output_size", env_result.tool_max_output_size);

    cJSON *sandbox = cJSON_CreateObject();
    build_sandbox_rw(sandbox, env_result.sandbox_root, env_result.sandbox_rw_paths);
    {
        gchar *workspace_path = g_build_filename(edc_home, "workspace", NULL);
        gchar **ro = split_paths(env_result.sandbox_ro_paths);
        cJSON *ro_arr = cJSON_CreateArray();
        if (ro)
        {
            for (int i = 0; ro[i]; i++)
                cJSON_AddItemToArray(ro_arr, cJSON_CreateString(ro[i]));
            g_strfreev(ro);
        }
        cJSON_AddItemToArray(ro_arr, cJSON_CreateString(workspace_path));
        cJSON_AddItemToObject(sandbox, "ro_paths", ro_arr);
        g_free(workspace_path);
    }
    cJSON_AddItemToObject(config, "sandbox", sandbox);

    cJSON *channel = cJSON_CreateObject();
    cJSON_AddStringToObject(channel, "type", ctype);

    if (strcmp(ctype, "email") == 0)
    {
        cJSON_AddStringToObject(channel, "imap_host", env_result.email.imap_host);
        cJSON_AddStringToObject(channel, "smtp_host", env_result.email.smtp_host);
        cJSON_AddStringToObject(channel, "user", env_result.email.user);

        gchar *encrypted_passwd = edc_secret_encrypt(env_result.email.passwd);
        cJSON_AddStringToObject(channel, "passwd", encrypted_passwd ? encrypted_passwd : "");
        g_free(encrypted_passwd);

        cJSON_AddStringToObject(channel, "target", env_result.email.target);
    }
    else if (strcmp(ctype, "telegram") == 0)
    {
        gchar *encrypted_bot_key = edc_secret_encrypt(env_result.telegram.bot_key);
        cJSON_AddStringToObject(channel, "bot_key", encrypted_bot_key ? encrypted_bot_key : "");
        g_free(encrypted_bot_key);

        cJSON_AddNumberToObject(channel, "user_id", env_result.telegram.user_id);
    }
    else if (strcmp(ctype, "feishu") == 0)
    {
        cJSON_AddStringToObject(channel, "app_id", env_result.feishu.app_id);

        gchar *encrypted_app_secret = edc_secret_encrypt(env_result.feishu.app_secret);
        cJSON_AddStringToObject(channel, "app_secret", encrypted_app_secret ? encrypted_app_secret : "");
        g_free(encrypted_app_secret);
    }
    else if (strcmp(ctype, "qq") == 0)
    {
        cJSON_AddStringToObject(channel, "app_id", env_result.qq.app_id);

        gchar *encrypted_app_secret = edc_secret_encrypt(env_result.qq.app_secret);
        cJSON_AddStringToObject(channel, "app_secret", encrypted_app_secret ? encrypted_app_secret : "");
        g_free(encrypted_app_secret);
    }

    cJSON_AddItemToObject(config, "channel", channel);

    g_free(encrypted_api_key);

    gchar *config_str = cJSON_Print(config);
    GError *error = NULL;
    if (!g_file_set_contents(conf_file, config_str, -1, &error))
    {
        log_error("save config to %s failed: %s", conf_file, error ? error->message : "Unknown");
        cJSON_Delete(config);
        g_free(config_str);
        if (error)
            g_error_free(error);
        return FALSE;
    }

    cJSON_Delete(config);
    g_free(config_str);

    return TRUE;
}

gboolean edc_init_env(const gchar *edc_home, gboolean setup)
{
    gchar *workspace = g_build_filename(edc_home, "workspace", NULL);
    gchar *conf_file = g_build_filename(edc_home, "config.json", NULL);

    gboolean ret = TRUE;
    if (!g_file_test(edc_home, G_FILE_TEST_IS_DIR))
    {
        if (g_mkdir_with_parents(edc_home, 0755) != 0)
        {
            log_error("Failed to create directory %s", edc_home);
            ret = FALSE;
            goto exit_;
        }
    }

    if (setup || !g_file_test(conf_file, G_FILE_TEST_IS_REGULAR))
    {
        if (!edc_init_env_conf_file(conf_file, edc_home))
        {
            log_error("Failed to initialize config file.");
            ret = FALSE;
            goto exit_;
        }
    }

    if (!edc_init_env_workspace(workspace))
    {
        log_error("Failed to initialize workspace.");
        ret = FALSE;
        goto exit_;
    }

exit_:
    g_free(workspace);
    g_free(conf_file);
    return ret;
}
