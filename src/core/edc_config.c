#include "edc_config.h"
#include "cJSON.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"
#include "edc_secret.h"
#include "log.h"
#include <errno.h>

sds edc_config_resolve_bc_home(void)
{
    sds home = edc_homedir();
    if (sdslen(home) == 0)
    {
        sdsfree(home);
#ifdef _WIN32
        char tmp[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmp) == 0)
            log_error("edc_homedir() failed, falling back to C:/.edgeclaw");
        home = sdsnew(tmp);
        sds result = edc_path_join(home, ".edgeclaw", NULL);
        sdsfree(home);
        return result;
#else
        log_error("edc_homedir() failed, falling back to /tmp/.edgeclaw");
        return sdsnew("/tmp/.edgeclaw");
#endif
    }
    sds result = edc_path_join(home, ".edgeclaw", NULL);
    sdsfree(home);
    return result;
}

struct edc_config *edc_config_load(const char *edc_home)
{
    EdcError *error = NULL;
    struct edc_config *config = NULL;

    sds path = edc_path_join(edc_home, "config.json", NULL);
    sds content = edc_file_read(path, &error);

    if (!content)
    {
        log_error("failed to load config file: %s", error->message);
        edc_error_free(error);
        goto exit_free_path;
    }

    cJSON *conf = cJSON_Parse(content);
    if (!conf)
    {
        log_error("failed to parse config file: %s", cJSON_GetErrorPtr());
        goto exit_free_content;
    }

    if (!conf || !cJSON_IsObject(conf))
    {
        log_error("parse %s as json failed.", content);
        goto exit_free_conf;
    }

    config = g_new0(struct edc_config, 1);
    if (!config)
    {
        log_error("failed to allocate memory for config");
        goto exit_free_conf;
    }

    config->edc_home = sdsnew(edc_home);

    cJSON *api_key = cJSON_GetObjectItem(conf, "api_key");
    if (!api_key || !cJSON_IsString(api_key))
    {
        log_error("api_key is not string type");
        goto exit_free_config;
    }

    if (!strlen(cJSON_GetStringValue(api_key)))
    {
        config->llm_api_conf.api_key = NULL;
    }
    else
    {
        sds _d = edc_secret_decrypt(cJSON_GetStringValue(api_key));
        if (!_d)
        {
            log_error("api_key decryption failed");
            goto exit_free_config;
        }
        config->llm_api_conf.api_key = _d;
    }

    cJSON *model = cJSON_GetObjectItem(conf, "model");
    if (!model || !cJSON_IsString(model))
    {
        log_error("model is not string type");
        goto exit_free_config;
    }
    config->llm_api_conf.model = sdsnew(cJSON_GetStringValue(model));

    cJSON *api_url = cJSON_GetObjectItem(conf, "api_url");
    if (!api_url || !cJSON_IsString(api_url))
    {
        log_error("api_url is not string type");
        goto exit_free_config;
    }
    config->llm_api_conf.api_url = sdsnew(cJSON_GetStringValue(api_url));

    cJSON *max_iter = cJSON_GetObjectItem(conf, "max_iter");
    if (!max_iter || !cJSON_IsNumber(max_iter))
    {
        log_error("max_iter is not number type");
        goto exit_free_config;
    }
    config->max_iter = max_iter->valueint;

    /* The optional tunables below are all materialized into config.json by
     * edc_init_env() (EDC_CONFIG_DEFAULT_* in edc_init_env.h is the single
     * source of truth). The fallbacks here only cover a key that was removed
     * from the file by hand. */
    cJSON *memory_compress_threshold = cJSON_GetObjectItem(conf, "memory_compress_threshold");
    if (!memory_compress_threshold || !cJSON_IsNumber(memory_compress_threshold))
    {
        config->memory_compress_threshold = EDC_CONFIG_DEFAULT_MEMORY_COMPRESS_THRESHOLD;
    }
    else
    {
        config->memory_compress_threshold = memory_compress_threshold->valueint;
    }

    cJSON *group_fold_msg_threshold = cJSON_GetObjectItem(conf, "group_fold_msg_threshold");
    if (!group_fold_msg_threshold || !cJSON_IsNumber(group_fold_msg_threshold))
    {
        config->group_fold_msg_threshold = EDC_CONFIG_DEFAULT_GROUP_FOLD_MSG_THRESHOLD;
    }
    else
    {
        config->group_fold_msg_threshold = group_fold_msg_threshold->valueint;
    }

    cJSON *tool_max_output_size = cJSON_GetObjectItem(conf, "tool_max_output_size");
    if (!tool_max_output_size || !cJSON_IsNumber(tool_max_output_size))
    {
        log_error("tool_max_output_size is not number type");
        goto exit_free_config;
    }
    config->tool_max_output_size = tool_max_output_size->valueint;

    /* An empty device_pty means "no serial link"; store NULL so callers can
     * simply test the pointer (main.c guards PPPoS init on it). */
    cJSON *device_pty = cJSON_GetObjectItem(conf, "device_pty");
    if (device_pty && cJSON_IsString(device_pty) && strlen(device_pty->valuestring) > 0)
        config->device_pty = sdsnew(device_pty->valuestring);

    cJSON *device_ip = cJSON_GetObjectItem(conf, "device_ip");
    if (device_ip && cJSON_IsString(device_ip) && strlen(device_ip->valuestring) > 0)
        config->device_ip = sdsnew(device_ip->valuestring);

    cJSON *device_port = cJSON_GetObjectItem(conf, "device_port");
    config->device_port =
        (device_port && cJSON_IsNumber(device_port)) ? device_port->valueint : EDC_CONFIG_DEFAULT_DEVICE_PORT;

    cJSON *device_baud = cJSON_GetObjectItem(conf, "device_baud");
    config->device_baud =
        (device_baud && cJSON_IsNumber(device_baud)) ? device_baud->valueint : EDC_CONFIG_DEFAULT_DEVICE_BAUD;

    cJSON *device_transport = cJSON_GetObjectItem(conf, "device_transport");
    if (device_transport && cJSON_IsString(device_transport) && strlen(device_transport->valuestring) > 0)
        config->device_transport = sdsnew(device_transport->valuestring);

    cJSON *channel = cJSON_GetObjectItem(conf, "channel");
    if (!channel || !cJSON_IsObject(channel))
    {
        log_warn("channel config missing, defaulting to 'adapter'");
        config->channel.type = edc_channel_adapter;
        goto after_channel;
    }

    cJSON *channel_type = cJSON_GetObjectItem(channel, "type");
    if (!channel_type || !cJSON_IsString(channel_type))
    {
        log_warn("channel.type missing, defaulting to 'adapter'");
        config->channel.type = edc_channel_adapter;
        goto after_channel;
    }

    const char *type = cJSON_GetStringValue(channel_type);
    if (strcmp(type, "adapter") == 0)
    {
        config->channel.type = edc_channel_adapter;
    }
    else if (strcmp(type, "cli") == 0)
    {
        log_error("channel type 'cli' is no longer supported, set channel.type to \"adapter\"");
        goto exit_free_config;
    }
    else
    {
        log_error("unknown channel type: %s, only 'adapter' is supported", type);
        goto exit_free_config;
    }

after_channel:

    config->workspace = edc_path_join(edc_home, "workspace", NULL);

    cJSON *sandbox = cJSON_GetObjectItem(conf, "sandbox");
    if (sandbox && cJSON_IsObject(sandbox))
    {
        cJSON *rw = cJSON_GetObjectItem(sandbox, "rw_paths");
        if (rw && cJSON_IsArray(rw) && cJSON_GetArraySize(rw) > 0)
        {
            int size = cJSON_GetArraySize(rw);
            config->sandbox_rw_paths = g_new0(sds, size + 1);
            for (int i = 0; i < size; i++)
                config->sandbox_rw_paths[i] = sdsnew(cJSON_GetStringValue(cJSON_GetArrayItem(rw, i)));
        }
        else
        {
            config->sandbox_rw_paths = g_new0(sds, 3 + 1);
            config->sandbox_rw_paths[0] = edc_path_join(edc_home, "sandbox", NULL);
            config->sandbox_rw_paths[1] = sdsnew("/tmp");
            config->sandbox_rw_paths[2] = sdsnew("/dev");
        }

        cJSON *ro = cJSON_GetObjectItem(sandbox, "ro_paths");
        if (ro && cJSON_IsArray(ro) && cJSON_GetArraySize(ro) > 0)
        {
            int size = cJSON_GetArraySize(ro);
            config->sandbox_ro_paths = g_new0(sds, size + 1);
            for (int i = 0; i < size; i++)
                config->sandbox_ro_paths[i] = sdsnew(cJSON_GetStringValue(cJSON_GetArrayItem(ro, i)));
        }
        else
        {
            config->sandbox_ro_paths = g_new0(sds, 10 + 1);
            config->sandbox_ro_paths[0] = sdsnew("/");
            config->sandbox_ro_paths[1] = sdsnew("/usr");
            config->sandbox_ro_paths[2] = sdsnew("/lib");
            config->sandbox_ro_paths[3] = sdsnew("/lib32");
            config->sandbox_ro_paths[4] = sdsnew("/lib64");
            config->sandbox_ro_paths[5] = sdsnew("/libx32");
            config->sandbox_ro_paths[6] = sdsnew("/bin");
            config->sandbox_ro_paths[7] = sdsnew("/sbin");
            config->sandbox_ro_paths[8] = sdsnew("/etc");
            config->sandbox_ro_paths[9] = sdsnew("/proc");
        }
    }
    else
    {
        config->sandbox_rw_paths = g_new0(sds, 3 + 1);
        config->sandbox_rw_paths[0] = edc_path_join(edc_home, "sandbox", NULL);
        config->sandbox_rw_paths[1] = sdsnew("/tmp");
        config->sandbox_rw_paths[2] = sdsnew("/dev");

        config->sandbox_ro_paths = g_new0(sds, 10 + 1);
        config->sandbox_ro_paths[0] = sdsnew("/");
        config->sandbox_ro_paths[1] = sdsnew("/usr");
        config->sandbox_ro_paths[2] = sdsnew("/lib");
        config->sandbox_ro_paths[3] = sdsnew("/lib32");
        config->sandbox_ro_paths[4] = sdsnew("/lib64");
        config->sandbox_ro_paths[5] = sdsnew("/libx32");
        config->sandbox_ro_paths[6] = sdsnew("/bin");
        config->sandbox_ro_paths[7] = sdsnew("/sbin");
        config->sandbox_ro_paths[8] = sdsnew("/etc");
        config->sandbox_ro_paths[9] = sdsnew("/proc");
    }

    /* The agent's own workspace must be writable: the model edits files there
     * (AGENTS.md, skills/, ...). Add it to rw_paths -- which grants read too --
     * instead of ro_paths, so it is writable in every configuration, not only
     * when the setup wizard happened to list it under rw_paths. rw_paths[0]
     * stays the launch directory (prepended below), so the sandbox root is
     * unchanged. */
    {
        int rw_count = 0;
        if (config->sandbox_rw_paths)
        {
            while (config->sandbox_rw_paths[rw_count])
                rw_count++;
        }
        config->sandbox_rw_paths = realloc(config->sandbox_rw_paths, (rw_count + 2) * sizeof(sds));
        config->sandbox_rw_paths[rw_count] = sdsnew(config->workspace);
        config->sandbox_rw_paths[rw_count + 1] = NULL;
    }

    /* The whole filesystem is readable: tools need to read system files
     * (/etc, /run, /proc, ...; note /etc/resolv.conf is often a symlink into
     * /run, which DNS resolution needs) and the user's own files. Writes stay
     * confined to the writable set above. '/' may already be present from the
     * built-in default, so only add it when missing. */
    {
        bool have_root = false;
        for (int i = 0; config->sandbox_ro_paths && config->sandbox_ro_paths[i]; i++)
        {
            if (strcmp(config->sandbox_ro_paths[i], "/") == 0)
            {
                have_root = true;
                break;
            }
        }
        if (!have_root)
        {
            int ro_count = 0;
            if (config->sandbox_ro_paths)
            {
                while (config->sandbox_ro_paths[ro_count])
                    ro_count++;
            }
            config->sandbox_ro_paths = realloc(config->sandbox_ro_paths, (ro_count + 2) * sizeof(sds));
            config->sandbox_ro_paths[ro_count] = sdsnew("/");
            config->sandbox_ro_paths[ro_count + 1] = NULL;
        }
    }

    /* Prepend the directory edgeclaw was launched from to rw_paths so that the
     * current directory is always writable (even with the filesystem sandbox
     * enabled) and becomes the sandbox root / default working directory for
     * tool execution. rw_paths[0] below is resolved into sandbox_root_real. */
    {
        sds cwd = edc_cwd();
        if (sdslen(cwd) > 0)
        {
            char *cwd_real = realpath(cwd, NULL);
            const char *cwd_key = cwd_real ? cwd_real : cwd;
            bool duplicate = false;
            for (int i = 0; config->sandbox_rw_paths && config->sandbox_rw_paths[i] && !duplicate; i++)
            {
                char *entry_real = realpath(config->sandbox_rw_paths[i], NULL);
                const char *entry_key = entry_real ? entry_real : config->sandbox_rw_paths[i];
                if (strcmp(cwd_key, entry_key) == 0)
                    duplicate = true;
                if (entry_real)
                    free(entry_real);
            }
            if (!duplicate)
            {
                int n = 0;
                while (config->sandbox_rw_paths && config->sandbox_rw_paths[n])
                    n++;
                config->sandbox_rw_paths = realloc(config->sandbox_rw_paths, (size_t)(n + 2) * sizeof(sds));
                for (int i = n; i > 0; i--)
                    config->sandbox_rw_paths[i] = config->sandbox_rw_paths[i - 1];
                config->sandbox_rw_paths[0] = sdsnew(cwd_key);
                config->sandbox_rw_paths[n + 1] = NULL;
            }
            if (cwd_real)
                free(cwd_real);
        }
        sdsfree(cwd);
    }

    if (config->sandbox_rw_paths && config->sandbox_rw_paths[0])
    {
        edc_dir_create_with_parents(config->sandbox_rw_paths[0], 0755, NULL);
        char *rp = realpath(config->sandbox_rw_paths[0], NULL);
        config->sandbox_root_real = rp ? sdsnew(rp) : NULL;
        if (rp)
            free(rp);
        if (!config->sandbox_root_real)
        {
            log_error("edc_config: cannot resolve sandbox root '%s': %s", config->sandbox_rw_paths[0], strerror(errno));
        }
    }

    cJSON_Delete(conf);
    sdsfree(content);
    sdsfree(path);

    return config;

exit_free_config:
    free(config);
exit_free_conf:
    cJSON_Delete(conf);
exit_free_content:
    sdsfree(content);
exit_free_path:
    sdsfree(path);

    return NULL;
}

void edc_config_free(struct edc_config *config)
{
    if (!config)
        return;

    sdsfree(config->llm_api_conf.model);
    sdsfree(config->llm_api_conf.api_key);
    sdsfree(config->llm_api_conf.api_url);
    sdsfree(config->edc_home);
    sdsfree(config->device_pty);
    sdsfree(config->device_ip);
    sdsfree(config->device_transport);
    sdsfree(config->workspace);
    sdsfree(config->sandbox_root_real);
    for (int _i = 0; config->sandbox_ro_paths && config->sandbox_ro_paths[_i]; _i++)
        sdsfree(config->sandbox_ro_paths[_i]);
    free(config->sandbox_ro_paths);
    for (int _i = 0; config->sandbox_rw_paths && config->sandbox_rw_paths[_i]; _i++)
        sdsfree(config->sandbox_rw_paths[_i]);
    free(config->sandbox_rw_paths);
    free(config);
}

bool edc_config_update_llm(const char *edc_home, const char *api_url, const char *model, const char *api_key)
{
    if (!edc_home)
        return false;

    sds path = edc_path_join(edc_home, "config.json", NULL);
    EdcError *error = NULL;
    sds contents = edc_file_read(path, &error);
    if (!contents)
    {
        log_error("edc_config: cannot read %s: %s", path, error ? error->message : "unknown");
        if (error)
            edc_error_free(error);
        sdsfree(path);
        return false;
    }

    cJSON *conf = cJSON_Parse(contents);
    sdsfree(contents);
    if (!conf || !cJSON_IsObject(conf))
    {
        if (conf)
            cJSON_Delete(conf);
        sdsfree(path);
        return false;
    }

    if (api_url && api_url[0])
    {
        cJSON_DeleteItemFromObject(conf, "api_url");
        cJSON_AddStringToObject(conf, "api_url", api_url);
    }
    if (model && model[0])
    {
        cJSON_DeleteItemFromObject(conf, "model");
        cJSON_AddStringToObject(conf, "model", model);
    }
    if (api_key && api_key[0])
    {
        sds plain = sdsnew(api_key);
        sds enc = edc_secret_encrypt(plain);
        sdsfree(plain);
        cJSON_DeleteItemFromObject(conf, "api_key");
        cJSON_AddStringToObject(conf, "api_key", enc ? enc : "");
        sdsfree(enc);
    }

    char *out = cJSON_Print(conf);
    cJSON_Delete(conf);
    if (!out)
    {
        sdsfree(path);
        return false;
    }

    bool ok = edc_file_write(path, out, -1, &error);
    if (!ok)
    {
        log_error("edc_config: cannot write %s: %s", path, error ? error->message : "unknown");
        if (error)
            edc_error_free(error);
    }
    cJSON_free(out);
    sdsfree(path);
    return ok;
}
