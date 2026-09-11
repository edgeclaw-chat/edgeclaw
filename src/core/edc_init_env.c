#include "edc_init_env.h"
#include "cJSON.h"
#include "edc_bundled_workspace.h"
#include "edc_compat.h"
#include "edc_file_utils.h"
#include "edc_secret.h"
#include "edc_terminal.h"
#include "log.h"
#include "sds.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Device transport type options */
static const char *device_transport_types[] = {"socket", "tty"};
#define DEVICE_TRANSPORT_TYPE_COUNT 2

/* --- Helper functions --- */

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

    char buf[4096];
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) == NULL)
    {
        return NULL;
    }

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
        buf[--len] = '\0';

    if (buf[0] == '\0' && default_val && default_val[0] != '\0')
        return sdsnew(default_val);

    return sdsnew(buf);
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
#ifdef _WIN32
    char buf[4096];
    if (fgets(buf, sizeof(buf), stdin) != NULL)
    {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
            buf[len - 1] = '\0';
        result = sdsnew(buf);
    }
#else
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
#endif

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

/* Default "additional writable paths" — everything after the sandbox root. */
static sds default_sandbox_rw_extra(const char *edc_home)
{
    return sdscatprintf(sdsempty(), "%s/workspace/, /tmp, /dev", edc_home);
}

/* Default read-only paths. The whole filesystem is readable so tools can read
 * system files (/etc, /run, /proc, ...) and the user's own files; writes stay
 * confined to the writable set below. The loader also ensures '/' is present
 * and adds the workspace to the writable set. */
static sds default_sandbox_ro(const char *edc_home)
{
    (void)edc_home;
    return sdsnew("/");
}

/* Parse existing config.json into a edc_init_env_result.
 * Missing keys are filled with the canonical defaults so the interactive setup
 * always shows a complete picture. */
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
    result->max_iter = (item && cJSON_IsNumber(item)) ? item->valueint : EDC_CONFIG_DEFAULT_MAX_ITER;

    item = cJSON_GetObjectItem(json, "memory_compress_threshold");
    result->memory_compress_threshold =
        (item && cJSON_IsNumber(item)) ? item->valueint : EDC_CONFIG_DEFAULT_MEMORY_COMPRESS_THRESHOLD;

    item = cJSON_GetObjectItem(json, "group_fold_msg_threshold");
    result->group_fold_msg_threshold =
        (item && cJSON_IsNumber(item)) ? item->valueint : EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD;

    item = cJSON_GetObjectItem(json, "tool_max_output_size");
    result->tool_max_output_size =
        (item && cJSON_IsNumber(item)) ? item->valueint : EDC_CONFIG_DEFAULT_TOOL_MAX_OUTPUT_SIZE;

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
    result->device_port = (item && cJSON_IsNumber(item)) ? item->valueint : EDC_CONFIG_DEFAULT_DEVICE_PORT;

    item = cJSON_GetObjectItem(json, "device_baud");
    result->device_baud = (item && cJSON_IsNumber(item)) ? item->valueint : EDC_CONFIG_DEFAULT_DEVICE_BAUD;

    item = cJSON_GetObjectItem(json, "device_transport");
    if (item && cJSON_IsString(item) && item->valuestring[0])
        result->device_transport = sdsnew(item->valuestring);
    else
        result->device_transport = sdsnew(EDC_CONFIG_DEFAULT_DEVICE_TRANSPORT);

    cJSON *channel = cJSON_GetObjectItem(json, "channel");
    (void)channel; /* 通道固定为 adapter, 无需从旧配置读取 */

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
    const char *def_api_url =
        (defaults && sdslen(defaults->api_url) > 0) ? defaults->api_url : EDC_CONFIG_DEFAULT_API_URL;
    sds val = prompt_with_default("API URL", def_api_url);
    if (!val)
        return -1;
    result->api_url = val;

    /* Model */
    const char *def_model = (defaults && sdslen(defaults->model) > 0) ? defaults->model : EDC_CONFIG_DEFAULT_MODEL;
    val = prompt_with_default("Model", def_model);
    if (!val)
        return -1;
    result->model = val;

    /* API Key (password) */
    val = prompt_password("API Key", (defaults && sdslen(defaults->api_key) > 0) ? defaults->api_key : NULL);
    if (!val)
        return -1;
    result->api_key = val;

    /* Max Iterations: -1 = unlimited; positive N = at most N LLM/tool rounds per input */
    int ival = prompt_int("Max Iterations",
                          defaults && defaults->max_iter > 0 ? defaults->max_iter : EDC_CONFIG_DEFAULT_MAX_ITER);
    if (ival < -1)
        return -1;
    result->max_iter = ival;

    /* Memory Compress Threshold */
    ival = prompt_int("Memory Compress Threshold", defaults && defaults->memory_compress_threshold > 0
                                                       ? defaults->memory_compress_threshold
                                                       : EDC_CONFIG_DEFAULT_MEMORY_COMPRESS_THRESHOLD);
    if (ival < 0)
        return -1;
    result->memory_compress_threshold = ival;

    /* Group Fold Message Threshold (L1: only conversation count drives fold) */
    ival = prompt_int("Group Fold Message Threshold", defaults && defaults->group_fold_msg_threshold > 0
                                                          ? defaults->group_fold_msg_threshold
                                                          : EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD);
    if (ival < 0)
        return -1;
    result->group_fold_msg_threshold = ival;

    /* Tool Max Output Size */
    ival = prompt_int("Tool Max Output Size", defaults && defaults->tool_max_output_size > 0
                                                  ? defaults->tool_max_output_size
                                                  : EDC_CONFIG_DEFAULT_TOOL_MAX_OUTPUT_SIZE);
    if (ival < 0)
        return -1;
    result->tool_max_output_size = ival;

    /* Sandbox Configuration */
    printf("\n--- Sandbox Configuration ---\n");

    {
        sds default_sandbox = edc_path_join(edc_home, "sandbox", NULL);
        const char *def = (defaults && sdslen(defaults->sandbox_root) > 0) ? defaults->sandbox_root : default_sandbox;
        val = prompt_with_default("Sandbox Root (working directory, writable)", def);
        sdsfree(default_sandbox);
        if (!val)
            return -1;
        result->sandbox_root = val;
    }

    {
        sds def_rw_str = default_sandbox_rw_extra(edc_home);
        const char *def_rw =
            (defaults && sdslen(defaults->sandbox_rw_paths) > 0) ? defaults->sandbox_rw_paths : def_rw_str;
        val = prompt_with_default("  Additional writable paths (comma-separated)", def_rw);
        sdsfree(def_rw_str);
        if (!val)
            return -1;
        result->sandbox_rw_paths = val;
    }

    {
        sds def_ro_str = default_sandbox_ro(edc_home);
        const char *def_ro =
            (defaults && sdslen(defaults->sandbox_ro_paths) > 0) ? defaults->sandbox_ro_paths : def_ro_str;
        val = prompt_with_default("  Read-only paths (comma-separated)", def_ro);
        sdsfree(def_ro_str);
        if (!val)
            return -1;
        result->sandbox_ro_paths = val;
    }

    /* --- Device Shell Configuration (optional) --- */

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
        const char *def =
            (defaults && sdslen(defaults->device_ip) > 0) ? defaults->device_ip : EDC_CONFIG_DEFAULT_DEVICE_IP;
        val = prompt_with_default("Device IP", def);
        if (!val)
            return -1;
        result->device_ip = val;
    }

    ival = prompt_int("Device Port",
                      (defaults && defaults->device_port > 0) ? defaults->device_port : EDC_CONFIG_DEFAULT_DEVICE_PORT);
    if (ival < 0)
        return -1;
    result->device_port = ival;

    if (strcmp(result->device_transport, "tty") == 0)
    {
        ival = prompt_int("Device Baud Rate", (defaults && defaults->device_baud > 0) ? defaults->device_baud
                                                                                      : EDC_CONFIG_DEFAULT_DEVICE_BAUD);
        if (ival < 0)
            return -1;
        result->device_baud = ival;
    }
    else
    {
        /* Baud is meaningless without a serial link, but it is still written to
         * config.json (see below) to keep the panorama complete. */
        result->device_baud =
            (defaults && defaults->device_baud > 0) ? defaults->device_baud : EDC_CONFIG_DEFAULT_DEVICE_BAUD;
    }

    printf("\n===================================================\n");
    return 0;
}

/* Release the workspace tree embedded in the binary (see
 * edc_bundled_workspace.h) into dest_workspace. */
static bool edc_init_env_workspace(const char *dest_workspace)
{
    if (edc_file_is_dir(dest_workspace))
    {
        return true;
    }

    log_info("Releasing bundled workspace (%zu files) to %s", edc_bundled_workspace_count, dest_workspace);

    if (!edc_dir_create_with_parents(dest_workspace, 0755, NULL))
    {
        log_error("Failed to create workspace directory %s", dest_workspace);
        return false;
    }

    for (size_t i = 0; i < edc_bundled_workspace_count; i++)
    {
        const struct edc_bundled_file *file = &edc_bundled_workspace_entries[i];
        sds dest_path = edc_path_join(dest_workspace, file->path, NULL);

        /* Recreate the parent directories of nested entries (skills/<name>/...). */
        sds parent = edc_path_dirname(dest_path);
        bool parent_ok = edc_dir_create_with_parents(parent, 0755, NULL);
        sdsfree(parent);
        if (!parent_ok)
        {
            log_error("Failed to create parent directory for %s", dest_path);
            sdsfree(dest_path);
            return false;
        }

        EdcError *error = NULL;
        if (!edc_file_write(dest_path, (const char *)file->data, (ssize_t)file->size, &error))
        {
            log_error("Failed to write %s: %s", dest_path, error ? error->message : "Unknown");
            if (error)
                edc_error_free(error);
            sdsfree(dest_path);
            return false;
        }
        sdsfree(dest_path);
    }

    log_info("Bundled workspace released successfully");
    return true;
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
    sdsfree(result->device_pty);
    sdsfree(result->device_ip);
    sdsfree(result->device_transport);
}

/* ── Silent top-up of an existing config.json ───────────────────────────────
 *
 * config.json files written by older versions are missing keys that used to be
 * supplied by implicit "if absent, use default" fallbacks scattered around
 * edc_config.c (group_fold_msg_threshold, device_baud, device_pty, ...). Those
 * keys were therefore invisible and could not be edited.
 *
 * On startup we fill in every missing key with its canonical default so the
 * file always shows the full configuration panorama. Only absent keys are
 * added: existing values, key order and user-added keys are untouched, and the
 * file is rewritten only when something was actually missing, so a complete
 * config never churns.
 *
 * The mirror case is handled too: keys that lost their last consumer are
 * dropped (see drop_obsolete_keys), so a retired knob does not linger in
 * everyone's config.json forever. */

static void ensure_int_key(cJSON *obj, const char *key, int def, bool *changed)
{
    if (cJSON_GetObjectItem(obj, key))
        return;
    cJSON_AddNumberToObject(obj, key, def);
    *changed = true;
}

static void ensure_str_key(cJSON *obj, const char *key, const char *def, bool *changed)
{
    if (cJSON_GetObjectItem(obj, key))
        return;
    cJSON_AddStringToObject(obj, key, def);
    *changed = true;
}

/* Keys that older versions materialized but that no longer have any consumer.
 * They are dropped from existing files so config.json only advertises live
 * knobs (dead keys invite pointless edits). */
static void drop_obsolete_keys(cJSON *conf, bool *changed)
{
    static const char *const obsolete[] = {"load_n_histories", NULL};

    for (int i = 0; obsolete[i]; i++)
    {
        if (!cJSON_GetObjectItem(conf, obsolete[i]))
            continue;
        cJSON_DeleteItemFromObject(conf, obsolete[i]);
        *changed = true;
        log_info("config.json: dropped obsolete key '%s'", obsolete[i]);
    }
}

/* Make sure the sandbox object exists and carries both path arrays. */
static void ensure_sandbox(cJSON *conf, const char *edc_home, bool *changed)
{
    cJSON *sandbox = cJSON_GetObjectItem(conf, "sandbox");
    if (!sandbox || !cJSON_IsObject(sandbox))
    {
        if (sandbox)
            cJSON_DeleteItemFromObject(conf, "sandbox");
        sandbox = cJSON_AddObjectToObject(conf, "sandbox");
        *changed = true;
    }

    if (!cJSON_GetObjectItem(sandbox, "rw_paths"))
    {
        sds root = edc_path_join(edc_home, "sandbox", NULL);
        sds extra = default_sandbox_rw_extra(edc_home);
        build_sandbox_rw(sandbox, root, extra);
        sdsfree(root);
        sdsfree(extra);
        *changed = true;
    }

    if (!cJSON_GetObjectItem(sandbox, "ro_paths"))
    {
        sds workspace = edc_path_join(edc_home, "workspace", NULL);
        sds ro_defaults = default_sandbox_ro(edc_home);
        sds *ro = split_paths(ro_defaults);
        cJSON *arr = cJSON_CreateArray();
        if (ro)
        {
            for (int i = 0; ro[i]; i++)
                cJSON_AddItemToArray(arr, cJSON_CreateString(ro[i]));
            for (int i = 0; ro[i]; i++)
                sdsfree(ro[i]);
            free(ro);
        }
        cJSON_AddItemToArray(arr, cJSON_CreateString(workspace));
        cJSON_AddItemToObject(sandbox, "ro_paths", arr);
        sdsfree(ro_defaults);
        sdsfree(workspace);
        *changed = true;
    }
}

static void edc_init_env_conf_topup(const char *conf_file, const char *edc_home)
{
    if (!edc_file_is_regular(conf_file))
        return;

    EdcError *error = NULL;
    sds contents = edc_file_read(conf_file, &error);
    if (!contents)
    {
        log_error("read %s failed: %s", conf_file, error ? error->message : "Unknown");
        if (error)
            edc_error_free(error);
        return;
    }

    cJSON *conf = cJSON_Parse(contents);
    sdsfree(contents);
    if (!conf || !cJSON_IsObject(conf))
    {
        if (conf)
            cJSON_Delete(conf);
        log_warn("%s is not a JSON object, skipping config top-up", conf_file);
        return;
    }

    bool changed = false;

    ensure_str_key(conf, "api_url", EDC_CONFIG_DEFAULT_API_URL, &changed);
    ensure_str_key(conf, "model", EDC_CONFIG_DEFAULT_MODEL, &changed);
    ensure_int_key(conf, "max_iter", EDC_CONFIG_DEFAULT_MAX_ITER, &changed);
    ensure_int_key(conf, "memory_compress_threshold", EDC_CONFIG_DEFAULT_MEMORY_COMPRESS_THRESHOLD, &changed);
    ensure_int_key(conf, "group_fold_msg_threshold", EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD, &changed);
    ensure_int_key(conf, "tool_max_output_size", EDC_CONFIG_DEFAULT_TOOL_MAX_OUTPUT_SIZE, &changed);

    ensure_sandbox(conf, edc_home, &changed);

    cJSON *channel = cJSON_GetObjectItem(conf, "channel");
    if (!channel || !cJSON_IsObject(channel))
    {
        if (channel)
            cJSON_DeleteItemFromObject(conf, "channel");
        channel = cJSON_AddObjectToObject(conf, "channel");
        changed = true;
    }
    ensure_str_key(channel, "type", EDC_CONFIG_DEFAULT_CHANNEL_TYPE, &changed);

    ensure_str_key(conf, "device_transport", EDC_CONFIG_DEFAULT_DEVICE_TRANSPORT, &changed);
    ensure_str_key(conf, "device_ip", EDC_CONFIG_DEFAULT_DEVICE_IP, &changed);
    ensure_int_key(conf, "device_port", EDC_CONFIG_DEFAULT_DEVICE_PORT, &changed);
    ensure_int_key(conf, "device_baud", EDC_CONFIG_DEFAULT_DEVICE_BAUD, &changed);
    ensure_str_key(conf, "device_pty", EDC_CONFIG_DEFAULT_DEVICE_PTY, &changed);

    drop_obsolete_keys(conf, &changed);

    if (changed)
    {
        char *config_str = cJSON_Print(conf);
        EdcError *werr = NULL;
        if (!edc_file_write(conf_file, config_str, -1, &werr))
        {
            log_error("save config to %s failed: %s", conf_file, werr ? werr->message : "Unknown");
            if (werr)
                edc_error_free(werr);
        }
        else
        {
            log_info("config.json updated with default keys / obsolete keys removed");
        }
        cJSON_free(config_str);
    }

    cJSON_Delete(conf);
}

static bool edc_init_env_conf_file(const char *conf_file, const char *edc_home)
{
    bool is_create = !edc_file_is_regular(conf_file);
    sds edc_home_dir = edc_path_dirname(conf_file);

    if (!edc_secret_init(is_create, edc_home_dir))
    {
        log_error("Failed to initialize secret module");
        sdsfree(edc_home_dir);
        return false;
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
        return false;
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
    if (env_result.max_iter == 0 || env_result.max_iter < -1)
    {
        log_error("Max Iterations must be -1 (unlimited) or a positive integer.");
        goto exit_free_env;
    }
    if (env_result.tool_max_output_size <= 0)
    {
        log_error("Tool Max Output Size must be greater than 0.");
        goto exit_free_env;
    }

    /* Channel is fixed to adapter; no additional validation */

    /* Build JSON config */
    cJSON *config = cJSON_CreateObject();

    sds encrypted_api_key = edc_secret_encrypt(env_result.api_key);
    cJSON_AddStringToObject(config, "api_key", encrypted_api_key ? encrypted_api_key : "");
    sdsfree(encrypted_api_key);

    cJSON_AddStringToObject(config, "model", env_result.model);
    cJSON_AddStringToObject(config, "api_url", env_result.api_url);
    cJSON_AddNumberToObject(config, "max_iter", env_result.max_iter);
    cJSON_AddNumberToObject(config, "memory_compress_threshold", env_result.memory_compress_threshold);
    cJSON_AddNumberToObject(config, "group_fold_msg_threshold", env_result.group_fold_msg_threshold);
    cJSON_AddNumberToObject(config, "tool_max_output_size", env_result.tool_max_output_size);

    cJSON *sandbox = cJSON_CreateObject();
    build_sandbox_rw(sandbox, env_result.sandbox_root, env_result.sandbox_rw_paths);
    {
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
        cJSON_AddItemToObject(sandbox, "ro_paths", ro_arr);
    }
    cJSON_AddItemToObject(config, "sandbox", sandbox);

    cJSON *channel = cJSON_CreateObject();
    cJSON_AddStringToObject(channel, "type", "adapter");

    cJSON_AddItemToObject(config, "channel", channel);

    /* Device link. Every key is written unconditionally so that config.json
     * stays a complete panorama of the configurables: device_pty is empty when
     * the transport is a plain socket. */
    cJSON_AddStringToObject(config, "device_transport", env_result.device_transport);
    cJSON_AddStringToObject(config, "device_ip", env_result.device_ip);
    cJSON_AddNumberToObject(config, "device_port", env_result.device_port);
    cJSON_AddNumberToObject(config, "device_baud",
                            env_result.device_baud > 0 ? env_result.device_baud : EDC_CONFIG_DEFAULT_DEVICE_BAUD);
    cJSON_AddStringToObject(config, "device_pty",
                            (env_result.device_pty && sdslen(env_result.device_pty) > 0) ? env_result.device_pty : "");

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
    return true;

exit_free_env:
    edc_init_env_result_free(&env_result);
    return false;
}

bool edc_init_env(const char *edc_home, bool setup)
{
    sds workspace = edc_path_join(edc_home, "workspace", NULL);
    sds conf_file = edc_path_join(edc_home, "config.json", NULL);

    bool ret = true;
    if (!edc_file_is_dir(edc_home))
    {
        if (!edc_dir_create_with_parents(edc_home, 0755, NULL))
        {
            log_error("Failed to create directory %s", edc_home);
            ret = false;
            goto exit_;
        }
    }

    if (setup || !edc_file_is_regular(conf_file))
    {
        if (!edc_init_env_conf_file(conf_file, edc_home))
        {
            log_error("Failed to initialize config file.");
            ret = false;
            goto exit_;
        }
    }
    else
    {
        /* Existing config: materialize any key this version knows about but the
         * file predates, so config.json always shows the full panorama. */
        edc_init_env_conf_topup(conf_file, edc_home);
    }

    if (!edc_init_env_workspace(workspace))
    {
        log_error("Failed to initialize workspace.");
        ret = false;
        goto exit_;
    }

exit_:
    sdsfree(workspace);
    sdsfree(conf_file);
    return ret;
}
