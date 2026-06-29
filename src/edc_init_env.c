#include "edc_init_env.h"
#include "edc_secret.h"
#include "cJSON.h"
#include "linenoise.h"
#include "log.h"
#include <dirent.h>
#include <inttypes.h>
#include "edc_compat.h"
#include "edc_file_utils.h"
#include "edc_terminal.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Channel type options */
static const char *channel_types[] = {"cli", "telegram", "email", "feishu", "qq"};
#define CHANNEL_TYPE_COUNT 5

static const char *device_transport_types[] = {"socket", "tty"};
#define DEVICE_TRANSPORT_TYPE_COUNT 2

/* --- Helper functions --- */

/* Read a line with linenoise, applying a default if the user enters nothing.
 * Returns an sds (caller must sdsfree), or NULL on EOF/Ctrl-C. */
static sds prompt_with_default(const char *label, const char *default_val)
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
        return sdsnew(default_val);
    }

    sds result = sdsnew(line);
    free(line);
    return result;
}

/* Read a password (no echo). If default_val is non-empty, shows [***unchanged***]
 * and returns a copy of it when the user enters nothing.
 * Returns sds, or NULL on error. */
static sds prompt_password(const char *label, const char *default_val)
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

    struct edc_terminal_state saved = {{0}};
    if (edc_terminal_echo_off(STDIN_FILENO, &saved) != 0)
    {
        return NULL;
    }

    sds result = NULL;
    char *raw = NULL;
    size_t raw_cap = 0;
    ssize_t raw_len = getline(&raw, &raw_cap, stdin);
    if (raw_len != -1)
    {
        if (raw_len > 0 && raw[raw_len - 1] == '\n')
            raw[raw_len - 1] = '\0';
        result = sdsnew(raw);
    }
    free(raw);

    edc_terminal_echo_restore(STDIN_FILENO, &saved);
    fprintf(stdout, "\n");
    fflush(stdout);

    /* If user entered nothing and a default exists, keep the old value */
    if (result && sdslen(result) == 0 && default_val && default_val[0] != '\0')
    {
        sdsfree(result);
        return sdsnew(default_val);
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
        sds line = prompt_with_default(label, default_str);
        if (line == NULL)
        {
            return -1;
        }

        int val = 0;
        if (sscanf(line, "%d", &val) == 1)
        {
            sdsfree(line);
            return val;
        }

        fprintf(stderr, "  Invalid number, please try again.\n");
        sdsfree(line);
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
        sds line = prompt_with_default("Select channel", default_str);
        if (line == NULL)
        {
            return -1;
        }

        int choice = 0;
        if (sscanf(line, "%d", &choice) == 1 && choice >= 1 && choice <= CHANNEL_TYPE_COUNT)
        {
            sdsfree(line);
            return choice - 1;
        }

        sdsfree(line);
        fprintf(stderr, "  Invalid choice, enter 1-%d.\n", CHANNEL_TYPE_COUNT);
    }
}

/* Prompt for device transport type selection. Returns index, or -1 on cancel. */
static int prompt_device_transport(int default_idx)
{
    if (default_idx < 0 || default_idx >= DEVICE_TRANSPORT_TYPE_COUNT)
        default_idx = 0;

    printf("\n--- Device Transport ---\n");
    for (int i = 0; i < DEVICE_TRANSPORT_TYPE_COUNT; i++)
    {
        printf("  %d) %s\n", i + 1, device_transport_types[i]);
    }

    char default_str[32];
    snprintf(default_str, sizeof(default_str), "%d", default_idx + 1);

    while (1)
    {
        sds line = prompt_with_default("Select transport", default_str);
        if (line == NULL)
        {
            return -1;
        }

        int choice = 0;
        if (sscanf(line, "%d", &choice) == 1 && choice >= 1 && choice <= DEVICE_TRANSPORT_TYPE_COUNT)
        {
            sdsfree(line);
            return choice - 1;
        }

        sdsfree(line);
        fprintf(stderr, "  Invalid choice, enter 1-%d.\n", DEVICE_TRANSPORT_TYPE_COUNT);
    }
}

/* Parse existing config.json into a edc_init_env_result.
 * Returns a newly allocated struct, or NULL if the file doesn't exist or can't be parsed. */
static struct edc_init_env_result *load_existing_config(const char *conf_file)
{
    if (!edc_file_is_regular(conf_file))
        return NULL;

    EdcError *error = NULL;
    sds contents = edc_file_read(conf_file, &error);
    if (!contents)
    {
        if (error)
            edc_error_free(error);
        return NULL;
    }

    cJSON *json = cJSON_Parse(contents);
    sdsfree(contents);
    if (!json)
        return NULL;

    struct edc_init_env_result *result = EDC_MALLOC0(sizeof(*result));

    cJSON *item;
    item = cJSON_GetObjectItem(json, "api_key");
    if (item && cJSON_IsString(item))
    {
        sds decrypted = edc_secret_decrypt(item->valuestring);
        if (decrypted)
        {
            result->api_key = decrypted;
        }
    }

    item = cJSON_GetObjectItem(json, "api_url");
    if (item && cJSON_IsString(item))
        result->api_url = sdsnew(item->valuestring);

    item = cJSON_GetObjectItem(json, "model");
    if (item && cJSON_IsString(item))
        result->model = sdsnew(item->valuestring);

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
                    result->sandbox_root = sdsnew(first->valuestring);

                if (size > 1)
                {
                    sds joined = sdsempty();
                    for (int i = 1; i < size; i++)
                    {
                        cJSON *elem = cJSON_GetArrayItem(rw, i);
                        if (elem && cJSON_IsString(elem))
                        {
                            if (sdslen(joined) > 0)
                                joined = sdscat(joined, ", ");
                            joined = sdscat(joined, elem->valuestring);
                        }
                    }
                    result->sandbox_rw_paths = joined;
                }
            }
        }

        cJSON *ro = cJSON_GetObjectItem(item, "ro_paths");
        if (ro && cJSON_IsArray(ro))
        {
            int size = cJSON_GetArraySize(ro);
            if (size > 0)
            {
                sds joined = sdsempty();
                for (int i = 0; i < size; i++)
                {
                    cJSON *elem = cJSON_GetArrayItem(ro, i);
                    if (elem && cJSON_IsString(elem))
                    {
                        if (sdslen(joined) > 0)
                            joined = sdscat(joined, ", ");
                        joined = sdscat(joined, elem->valuestring);
                    }
                }
                result->sandbox_ro_paths = joined;
            }
        }
    }

    item = cJSON_GetObjectItem(json, "device_pty");
    if (item && cJSON_IsString(item))
        result->device_pty = sdsnew(item->valuestring);

    item = cJSON_GetObjectItem(json, "device_ip");
    if (item && cJSON_IsString(item))
        result->device_ip = sdsnew(item->valuestring);

    item = cJSON_GetObjectItem(json, "device_port");
    result->device_port = (item && cJSON_IsNumber(item)) ? item->valueint : 5000;

    item = cJSON_GetObjectItem(json, "device_baud");
    result->device_baud = (item && cJSON_IsNumber(item)) ? item->valueint : 115200;

    item = cJSON_GetObjectItem(json, "device_transport");
    if (item && cJSON_IsString(item))
        result->device_transport = sdsnew(item->valuestring);

    cJSON *channel = cJSON_GetObjectItem(json, "channel");
    if (channel)
    {
        item = cJSON_GetObjectItem(channel, "type");
        if (item && cJSON_IsString(item))
            result->channel_type = sdsnew(item->valuestring);

        const char *ctype = result->channel_type;

        if (strcmp(ctype, "email") == 0)
        {
            item = cJSON_GetObjectItem(channel, "imap_host");
            if (item && cJSON_IsString(item))
                result->email.imap_host = sdsnew(item->valuestring);

            item = cJSON_GetObjectItem(channel, "smtp_host");
            if (item && cJSON_IsString(item))
                result->email.smtp_host = sdsnew(item->valuestring);

            item = cJSON_GetObjectItem(channel, "user");
            if (item && cJSON_IsString(item))
                result->email.user = sdsnew(item->valuestring);

            item = cJSON_GetObjectItem(channel, "passwd");
            if (item && cJSON_IsString(item))
            {
                sds decrypted = edc_secret_decrypt(item->valuestring);
                if (decrypted)
                {
                    result->email.passwd = decrypted;
                }
            }

            item = cJSON_GetObjectItem(channel, "target");
            if (item && cJSON_IsString(item))
                result->email.target = sdsnew(item->valuestring);
        }
        else if (strcmp(ctype, "telegram") == 0)
        {
            item = cJSON_GetObjectItem(channel, "bot_key");
            if (item && cJSON_IsString(item))
            {
                sds decrypted = edc_secret_decrypt(item->valuestring);
                if (decrypted)
                {
                    result->telegram.bot_key = decrypted;
                }
            }

            item = cJSON_GetObjectItem(channel, "user_id");
            if (item && cJSON_IsNumber(item))
                result->telegram.user_id = (int64_t)item->valuedouble;
        }
        else if (strcmp(ctype, "feishu") == 0)
        {
            item = cJSON_GetObjectItem(channel, "app_id");
            if (item && cJSON_IsString(item))
                result->feishu.app_id = sdsnew(item->valuestring);

            item = cJSON_GetObjectItem(channel, "app_secret");
            if (item && cJSON_IsString(item))
            {
                sds decrypted = edc_secret_decrypt(item->valuestring);
                if (decrypted)
                {
                    result->feishu.app_secret = decrypted;
                }
            }
        }
        else if (strcmp(ctype, "qq") == 0)
        {
            item = cJSON_GetObjectItem(channel, "app_id");
            if (item && cJSON_IsString(item))
                result->qq.app_id = sdsnew(item->valuestring);

            item = cJSON_GetObjectItem(channel, "app_secret");
            if (item && cJSON_IsString(item))
            {
                sds decrypted = edc_secret_decrypt(item->valuestring);
                if (decrypted)
                {
                    result->qq.app_secret = decrypted;
                }
            }
        }
    }

    cJSON_Delete(json);
    return result;
}

static int edc_init_env_get_configure(struct edc_init_env_result *result, const struct edc_init_env_result *defaults,
                                     const char *edc_home)
{
    if (!result)
        return -1;

    memset(result, 0, sizeof(*result));

    printf("\n========== EdgeClaw Configuration Setup ==========\n");

    /* --- LLM API Configuration --- */
    printf("\n--- LLM API Configuration ---\n");

    /* API URL */
    const char *def_api_url = (defaults && sdslen(defaults->api_url) > 0)
                                  ? defaults->api_url
                                  : "https://open.bigmodel.cn/api/coding/paas/v4/chat/completions";
    sds val = prompt_with_default("API URL", def_api_url);
    if (!val)
        return -1;
    result->api_url = val;

    /* Model */
    const char *def_model = (defaults && sdslen(defaults->model) > 0) ? defaults->model : "glm-5";
    val = prompt_with_default("Model", def_model);
    if (!val)
        return -1;
    result->model = val;

    /* API Key (password) */
    val = prompt_password("API Key", (defaults && sdslen(defaults->api_key) > 0) ? defaults->api_key : NULL);
    if (!val)
        return -1;
    result->api_key = val;

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
                      defaults && defaults->memory_compress_threshold > 0 ? defaults->memory_compress_threshold : 200);
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
        sds default_sandbox = sdscatprintf(sdsempty(), "%s/sandbox", edc_home);
        const char *def = (defaults && sdslen(defaults->sandbox_root) > 0) ? defaults->sandbox_root : default_sandbox;
        val = prompt_with_default("Sandbox Root (working directory, writable)", def);
        sdsfree(default_sandbox);
        if (!val)
            return -1;
        result->sandbox_root = val;
    }

    {
        sds def_rw_str =
            sdscatprintf(sdsempty(), "%s/workspace/, /tmp, /dev", edc_home);
        const char *def_rw = (defaults && sdslen(defaults->sandbox_rw_paths) > 0) ? defaults->sandbox_rw_paths : def_rw_str;
        val = prompt_with_default("  Additional writable paths (comma-separated)", def_rw);
        sdsfree(def_rw_str);
        if (!val)
            return -1;
        result->sandbox_rw_paths = val;
    }

    {
        sds def_ro_str =
            sdscatprintf(sdsempty(), "%s/workspace/ssl, /usr, /lib, /lib32, /lib64, /libx32, /bin, /sbin, /etc, /proc",
                            edc_home);
        const char *def_ro = (defaults && sdslen(defaults->sandbox_ro_paths) > 0)
                                 ? defaults->sandbox_ro_paths
                                 : def_ro_str;
        val = prompt_with_default("  Read-only paths (comma-separated)", def_ro);
        sdsfree(def_ro_str);
        if (!val)
            return -1;
        result->sandbox_ro_paths = val;
    }

    /* --- Channel Type --- */
    int default_channel = 0;
    if (defaults && sdslen(defaults->channel_type) > 0)
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
    result->channel_type = sdsnew(channel_types[channel_idx]);

    /* --- Channel-specific settings --- */
    const char *ctype = result->channel_type;

    if (strcmp(ctype, "email") == 0)
    {
        printf("\n--- Email Settings ---\n");
        bool same_chan = (defaults && strcmp(defaults->channel_type, "email") == 0);

        const char *def = (same_chan && sdslen(defaults->email.imap_host) > 0) ? defaults->email.imap_host : "imap.qq.com";
        val = prompt_with_default("IMAP Host", def);
        if (!val)
            return -1;
        result->email.imap_host = val;

        def = (same_chan && sdslen(defaults->email.smtp_host) > 0) ? defaults->email.smtp_host : "smtp.qq.com";
        val = prompt_with_default("SMTP Host", def);
        if (!val)
            return -1;
        result->email.smtp_host = val;

        def = (same_chan && sdslen(defaults->email.user) > 0) ? defaults->email.user : "";
        val = prompt_with_default("Email User", def);
        if (!val)
            return -1;
        result->email.user = val;

        val =
            prompt_password("Email Password", (same_chan && sdslen(defaults->email.passwd) > 0) ? defaults->email.passwd : NULL);
        if (!val)
            return -1;
        result->email.passwd = val;

        def = (same_chan && sdslen(defaults->email.target) > 0) ? defaults->email.target : "";
        val = prompt_with_default("Target Email", def);
        if (!val)
            return -1;
        result->email.target = val;
    }
    else if (strcmp(ctype, "telegram") == 0)
    {
        printf("\n--- Telegram Settings ---\n");
        bool same_chan = (defaults && strcmp(defaults->channel_type, "telegram") == 0);

        val = prompt_password("Bot Key",
                              (same_chan && sdslen(defaults->telegram.bot_key) > 0) ? defaults->telegram.bot_key : NULL);
        if (!val)
            return -1;
        result->telegram.bot_key = val;

        /* User ID is an integer */
        while (1)
        {
            char uid_buf[64] = "";
            const char *uid_def = uid_buf;
            if (same_chan && defaults->telegram.user_id != 0)
            {
                snprintf(uid_buf, sizeof(uid_buf), "%" PRId64, defaults->telegram.user_id);
            }
            val = prompt_with_default("User ID", uid_def);
            if (!val)
                return -1;
            int64_t uid = 0;
            if (sscanf(val, "%" PRId64, &uid) == 1 && uid != 0)
            {
                result->telegram.user_id = uid;
                sdsfree(val);
                break;
            }
            fprintf(stderr, "  Invalid number, please try again.\n");
            sdsfree(val);
        }
    }
    else if (strcmp(ctype, "feishu") == 0)
    {
        printf("\n--- Feishu Settings ---\n");
        bool same_chan = (defaults && strcmp(defaults->channel_type, "feishu") == 0);

        const char *def = (same_chan && sdslen(defaults->feishu.app_id) > 0) ? defaults->feishu.app_id : "";
        val = prompt_with_default("Feishu App ID", def);
        if (!val)
            return -1;
        result->feishu.app_id = val;

        val = prompt_password("Feishu App Secret",
                              (same_chan && sdslen(defaults->feishu.app_secret) > 0) ? defaults->feishu.app_secret : NULL);
        if (!val)
            return -1;
        result->feishu.app_secret = val;
    }
    else if (strcmp(ctype, "qq") == 0)
    {
        printf("\n--- QQ Settings ---\n");
        bool same_chan = (defaults && strcmp(defaults->channel_type, "qq") == 0);

        const char *def = (same_chan && sdslen(defaults->qq.app_id) > 0) ? defaults->qq.app_id : "";
        val = prompt_with_default("QQ App ID", def);
        if (!val)
            return -1;
        result->qq.app_id = val;

        val = prompt_password("QQ App Secret",
                              (same_chan && sdslen(defaults->qq.app_secret) > 0) ? defaults->qq.app_secret : NULL);
        if (!val)
            return -1;
        result->qq.app_secret = val;
    }

    printf("\n--- Device Shell Configuration (optional) ---\n");

    int default_transport = 0;
    if (defaults && sdslen(defaults->device_transport) > 0)
    {
        for (int i = 0; i < DEVICE_TRANSPORT_TYPE_COUNT; i++)
        {
            if (strcmp(defaults->device_transport, device_transport_types[i]) == 0)
            {
                default_transport = i;
                break;
            }
        }
    }
    int transport_idx = prompt_device_transport(default_transport);
    if (transport_idx < 0)
        return -1;
    result->device_transport = sdsnew(device_transport_types[transport_idx]);

    if (strcmp(result->device_transport, "tty") == 0)
    {
        const char *def = (defaults && sdslen(defaults->device_pty) > 0) ? defaults->device_pty : "";
        val = prompt_with_default("Device PTY/Serial Path", def);
        if (!val)
            return -1;
        result->device_pty = val;
    }

    {
        const char *def = (defaults && sdslen(defaults->device_ip) > 0) ? defaults->device_ip : "192.168.4.1";
        val = prompt_with_default("Device IP", def);
        if (!val)
            return -1;
        result->device_ip = val;
    }

    ival = prompt_int("Device Port",
                      (defaults && defaults->device_port > 0) ? defaults->device_port : 5000);
    if (ival < 0) return -1;
    result->device_port = ival;

    if (strcmp(result->device_transport, "tty") == 0)
    {
        ival = prompt_int("Device Baud Rate",
                          (defaults && defaults->device_baud > 0) ? defaults->device_baud : 115200);
        if (ival < 0) return -1;
        result->device_baud = ival;
    }

    printf("\n===================================================\n");
    return 0;
}

/* Copy directory recursively */
static bool copy_directory_recursive(const char *src_dir, const char *dest_dir)
{
    DIR *dir = opendir(src_dir);
    if (!dir)
    {
        log_error("Failed to open source directory %s", src_dir);
        return FALSE;
    }

    if (!edc_dir_create_with_parents(dest_dir, 0755, NULL))
    {
        log_error("Failed to create destination directory %s", dest_dir);
        closedir(dir);
        return FALSE;
    }

    struct dirent *entry;
    bool success = TRUE;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        sds src_path = edc_path_join(src_dir, entry->d_name, NULL);
        sds dest_path = edc_path_join(dest_dir, entry->d_name, NULL);

        struct stat st;
        if (lstat(src_path, &st) != 0)
        {
            log_error("Failed to stat %s", src_path);
            success = FALSE;
            sdsfree(src_path);
            sdsfree(dest_path);
            break;
        }

        if (S_ISDIR(st.st_mode))
        {
            if (!copy_directory_recursive(src_path, dest_path))
            {
                success = FALSE;
                sdsfree(src_path);
                sdsfree(dest_path);
                break;
            }
        }
        else
        {
            EdcError *error = NULL;
            sds contents = edc_file_read(src_path, &error);

            if (!contents)
            {
                log_error("Failed to read %s: %s", src_path, error ? error->message : "Unknown");
                if (error)
                    edc_error_free(error);
                success = FALSE;
                sdsfree(src_path);
                sdsfree(dest_path);
                break;
            }

            if (!edc_file_write(dest_path, contents, -1, &error))
            {
                log_error("Failed to write %s: %s", dest_path, error ? error->message : "Unknown");
                if (error)
                    edc_error_free(error);
                success = FALSE;
                sdsfree(contents);
                sdsfree(src_path);
                sdsfree(dest_path);
                break;
            }
            sdsfree(contents);
        }

        sdsfree(src_path);
        sdsfree(dest_path);
    }

    closedir(dir);
    return success;
}

static bool edc_init_env_workspace(const char *dest_workspace)
{
    if (edc_file_is_dir(dest_workspace))
    {
        return TRUE;
    }

    sds current_dir = edc_cwd();
    sds src_workspace = edc_path_join(current_dir, "workspace", NULL);
    sdsfree(current_dir);

    if (!edc_file_is_dir(src_workspace))
    {
        log_error("Source workspace directory %s does not exist", src_workspace);
        sdsfree(src_workspace);
        return FALSE;
    }

    log_info("Copying workspace from %s to %s", src_workspace, dest_workspace);

    bool success = copy_directory_recursive(src_workspace, dest_workspace);
    sdsfree(src_workspace);

    if (!success)
    {
        log_error("Failed to copy workspace directory");
        return FALSE;
    }

    log_info("Workspace directory copied successfully");
    return TRUE;
}

static sds *split_paths(const char *input)
{
    if (!input || !input[0])
        return NULL;

    int count;
    sds *parts = sdssplitlen(input, (ssize_t)strlen(input), ",", 1, &count);
    if (!parts || !parts[0])
        return NULL;

    int valid = 0;
    for (int i = 0; i < count; i++)
    {
        parts[i] = sdstrim(parts[i], " \t\r\n");
        if (parts[i][0])
            valid++;
    }

    if (valid == 0)
    {
        sdsfreesplitres(parts, count);
        return NULL;
    }

    sds *result = EDC_NEW0(sds, valid + 1);
    int idx = 0;
    for (int i = 0; i < count; i++)
    {
        if (parts[i][0])
            result[idx++] = sdsdup(parts[i]);
    }
    sdsfreesplitres(parts, count);
    return result;
}

static void build_sandbox_rw(cJSON *sandbox, const char *root, const char *rw_paths_str)
{
    cJSON *arr = cJSON_CreateArray();
    if (root && root[0])
        cJSON_AddItemToArray(arr, cJSON_CreateString(root));

    sds *extra = split_paths(rw_paths_str);
    if (extra)
    {
        for (int i = 0; extra[i]; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(extra[i]));
        for (int i = 0; extra[i]; i++)
            sdsfree(extra[i]);
        free(extra);
    }

    cJSON_AddItemToObject(sandbox, "rw_paths", arr);
}

void edc_init_env_result_free(struct edc_init_env_result *result)
{
    if (!result)
        return;

    sdsfree(result->api_key);
    sdsfree(result->api_url);
    sdsfree(result->model);
    sdsfree(result->sandbox_root);
    sdsfree(result->sandbox_rw_paths);
    sdsfree(result->sandbox_ro_paths);
    sdsfree(result->channel_type);
    sdsfree(result->device_pty);
    sdsfree(result->device_ip);
    sdsfree(result->device_transport);

    sdsfree(result->email.imap_host);
    sdsfree(result->email.smtp_host);
    sdsfree(result->email.user);
    sdsfree(result->email.passwd);
    sdsfree(result->email.target);

    sdsfree(result->telegram.bot_key);

    sdsfree(result->feishu.app_id);
    sdsfree(result->feishu.app_secret);

    sdsfree(result->qq.app_id);
    sdsfree(result->qq.app_secret);
}

static bool edc_init_env_conf_file(const char *conf_file, const char *edc_home)
{
    bool is_create = !edc_file_is_regular(conf_file);
    sds edc_home_dir = edc_path_dirname(conf_file);

    if (!edc_secret_init(is_create, edc_home_dir))
    {
        log_error("Failed to initialize secret module");
        sdsfree(edc_home_dir);
        return FALSE;
    }
    sdsfree(edc_home_dir);

    struct edc_init_env_result *existing = load_existing_config(conf_file);

    struct edc_init_env_result env_result;
    memset(&env_result, 0, sizeof(env_result));

    if (edc_init_env_get_configure(&env_result, existing, edc_home) != 0)
    {
        log_error("Configuration cancelled or failed.");
        edc_init_env_result_free(existing);
        free(existing);
        return FALSE;
    }
    edc_init_env_result_free(existing);
    free(existing);

    /* Validate required fields */
    if (sdslen(env_result.api_key) == 0)
    {
        log_error("API Key is required.");
        goto exit_free_env;
    }
    if (sdslen(env_result.api_url) == 0)
    {
        log_error("API URL is required.");
        goto exit_free_env;
    }
    if (sdslen(env_result.model) == 0)
    {
        log_error("Model is required.");
        goto exit_free_env;
    }
    if (env_result.max_iter <= 0)
    {
        log_error("Max Iterations must be greater than 0.");
        goto exit_free_env;
    }
    if (env_result.load_n_histories < 0)
    {
        log_error("Load N Histories must be >= 0.");
        goto exit_free_env;
    }
    if (env_result.tool_max_output_size <= 0)
    {
        log_error("Tool Max Output Size must be greater than 0.");
        goto exit_free_env;
    }

    /* Validate channel-specific fields */
    const char *ctype = env_result.channel_type;

    if (strcmp(ctype, "email") == 0)
    {
        if (sdslen(env_result.email.imap_host) == 0)
        {
            log_error("IMAP Host is required for email channel.");
            goto exit_free_env;
        }
        if (sdslen(env_result.email.smtp_host) == 0)
        {
            log_error("SMTP Host is required for email channel.");
            goto exit_free_env;
        }
        if (sdslen(env_result.email.user) == 0)
        {
            log_error("Email User is required for email channel.");
            goto exit_free_env;
        }
        if (sdslen(env_result.email.passwd) == 0)
        {
            log_error("Email Password is required for email channel.");
            goto exit_free_env;
        }
        if (sdslen(env_result.email.target) == 0)
        {
            log_error("Target Email is required for email channel.");
            goto exit_free_env;
        }
    }
    else if (strcmp(ctype, "telegram") == 0)
    {
        if (sdslen(env_result.telegram.bot_key) == 0)
        {
            log_error("Bot Key is required for telegram channel.");
            goto exit_free_env;
        }
        if (env_result.telegram.user_id == 0)
        {
            log_error("User ID is required for telegram channel.");
            goto exit_free_env;
        }
    }
    else if (strcmp(ctype, "feishu") == 0)
    {
        if (sdslen(env_result.feishu.app_id) == 0)
        {
            log_error("Feishu App ID is required for feishu channel.");
            goto exit_free_env;
        }
        if (sdslen(env_result.feishu.app_secret) == 0)
        {
            log_error("Feishu App Secret is required for feishu channel.");
            goto exit_free_env;
        }
    }
    else if (strcmp(ctype, "qq") == 0)
    {
        if (sdslen(env_result.qq.app_id) == 0)
        {
            log_error("QQ App ID is required for qq channel.");
            goto exit_free_env;
        }
        if (sdslen(env_result.qq.app_secret) == 0)
        {
            log_error("QQ App Secret is required for qq channel.");
            goto exit_free_env;
        }
    }

    /* Build JSON config */
    cJSON *config = cJSON_CreateObject();

    sds encrypted_api_key = edc_secret_encrypt(env_result.api_key);
    cJSON_AddStringToObject(config, "api_key", encrypted_api_key ? encrypted_api_key : "");
    sdsfree(encrypted_api_key);

    cJSON_AddStringToObject(config, "model", env_result.model);
    cJSON_AddStringToObject(config, "api_url", env_result.api_url);
    cJSON_AddNumberToObject(config, "max_iter", env_result.max_iter);
    cJSON_AddNumberToObject(config, "load_n_histories", env_result.load_n_histories);
    cJSON_AddNumberToObject(config, "memory_compress_threshold", env_result.memory_compress_threshold);
    cJSON_AddNumberToObject(config, "tool_max_output_size", env_result.tool_max_output_size);

    cJSON *sandbox = cJSON_CreateObject();
    build_sandbox_rw(sandbox, env_result.sandbox_root, env_result.sandbox_rw_paths);
    {
        sds workspace_path = edc_path_join(edc_home, "workspace", NULL);
        sds *ro = split_paths(env_result.sandbox_ro_paths);
        cJSON *ro_arr = cJSON_CreateArray();
        if (ro)
        {
            for (int i = 0; ro[i]; i++)
                cJSON_AddItemToArray(ro_arr, cJSON_CreateString(ro[i]));
            for (int i = 0; ro[i]; i++)
                sdsfree(ro[i]);
            free(ro);
        }
        cJSON_AddItemToArray(ro_arr, cJSON_CreateString(workspace_path));
        cJSON_AddItemToObject(sandbox, "ro_paths", ro_arr);
        sdsfree(workspace_path);
    }
    cJSON_AddItemToObject(config, "sandbox", sandbox);

    cJSON *channel = cJSON_CreateObject();
    cJSON_AddStringToObject(channel, "type", ctype);

    if (strcmp(ctype, "email") == 0)
    {
        cJSON_AddStringToObject(channel, "imap_host", env_result.email.imap_host);
        cJSON_AddStringToObject(channel, "smtp_host", env_result.email.smtp_host);
        cJSON_AddStringToObject(channel, "user", env_result.email.user);

        sds encrypted_passwd = edc_secret_encrypt(env_result.email.passwd);
        cJSON_AddStringToObject(channel, "passwd", encrypted_passwd ? encrypted_passwd : "");
        sdsfree(encrypted_passwd);

        cJSON_AddStringToObject(channel, "target", env_result.email.target);
    }
    else if (strcmp(ctype, "telegram") == 0)
    {
        sds encrypted_bot_key = edc_secret_encrypt(env_result.telegram.bot_key);
        cJSON_AddStringToObject(channel, "bot_key", encrypted_bot_key ? encrypted_bot_key : "");
        sdsfree(encrypted_bot_key);

        cJSON_AddNumberToObject(channel, "user_id", env_result.telegram.user_id);
    }
    else if (strcmp(ctype, "feishu") == 0)
    {
        cJSON_AddStringToObject(channel, "app_id", env_result.feishu.app_id);

        sds encrypted_app_secret = edc_secret_encrypt(env_result.feishu.app_secret);
        cJSON_AddStringToObject(channel, "app_secret", encrypted_app_secret ? encrypted_app_secret : "");
        sdsfree(encrypted_app_secret);
    }
    else if (strcmp(ctype, "qq") == 0)
    {
        cJSON_AddStringToObject(channel, "app_id", env_result.qq.app_id);

        sds encrypted_app_secret = edc_secret_encrypt(env_result.qq.app_secret);
        cJSON_AddStringToObject(channel, "app_secret", encrypted_app_secret ? encrypted_app_secret : "");
        sdsfree(encrypted_app_secret);
    }

    cJSON_AddItemToObject(config, "channel", channel);

    cJSON_AddStringToObject(config, "device_transport", env_result.device_transport);
    cJSON_AddStringToObject(config, "device_ip", env_result.device_ip);
    cJSON_AddNumberToObject(config, "device_port", env_result.device_port);

    if (env_result.device_pty && sdslen(env_result.device_pty) > 0)
    {
        cJSON_AddStringToObject(config, "device_pty", env_result.device_pty);
        cJSON_AddNumberToObject(config, "device_baud", env_result.device_baud);
    }

    char *config_str = cJSON_Print(config);
    EdcError *error = NULL;
    if (!edc_file_write(conf_file, config_str, -1, &error))
    {
        log_error("save config to %s failed: %s", conf_file, error ? error->message : "Unknown");
        cJSON_Delete(config);
        cJSON_free(config_str);
        if (error)
            edc_error_free(error);
        goto exit_free_env;
    }

    cJSON_Delete(config);
    cJSON_free(config_str);

    edc_init_env_result_free(&env_result);
    return TRUE;

exit_free_env:
    edc_init_env_result_free(&env_result);
    return FALSE;
}

bool edc_init_env(const char *edc_home, bool setup)
{
    sds workspace = edc_path_join(edc_home, "workspace", NULL);
    sds conf_file = edc_path_join(edc_home, "config.json", NULL);

    bool ret = TRUE;
    if (!edc_file_is_dir(edc_home))
    {
        if (!edc_dir_create_with_parents(edc_home, 0755, NULL))
        {
            log_error("Failed to create directory %s", edc_home);
            ret = FALSE;
            goto exit_;
        }
    }

    if (setup || !edc_file_is_regular(conf_file))
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
sdsfree(workspace);
sdsfree(conf_file);
    return ret;
}
