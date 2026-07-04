#include "edc_config.h"
#include "edc_error.h"
#include "edc_secret.h"
#include "edc_file_utils.h"
#include "cJSON.h"
#include "log.h"
#include "edc_compat.h"
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

static bool edc_config_parse_email_channel(struct edc_config *config, cJSON *channel)
{
    cJSON *imap_host = cJSON_GetObjectItem(channel, "imap_host");
    if (!imap_host || !cJSON_IsString(imap_host))
    {
        log_error("channel.imap_host is not string type");
        return false;
    }

    config->channel.email.imap_host = sdsnew(cJSON_GetStringValue(imap_host));

    cJSON *smtp_host = cJSON_GetObjectItem(channel, "smtp_host");
    if (!smtp_host || !cJSON_IsString(smtp_host))
    {
        log_error("channel.smtp_host is not string type");
        return false;
    }
    config->channel.email.smtp_host = sdsnew(cJSON_GetStringValue(smtp_host));

    cJSON *user = cJSON_GetObjectItem(channel, "user");
    if (!user || !cJSON_IsString(user))
    {
        log_error("channel.user is not string type");
        return false;
    }
    config->channel.email.user = sdsnew(cJSON_GetStringValue(user));

    cJSON *password = cJSON_GetObjectItem(channel, "passwd");
    if (!password || !cJSON_IsString(password))
    {
        log_error("channel.passwd is not string type");
        return false;
    }
    sds _d = edc_secret_decrypt(cJSON_GetStringValue(password));
    config->channel.email.passwd = _d;

    cJSON *target = cJSON_GetObjectItem(channel, "target");
    if (!target || !cJSON_IsString(target))
    {
        log_error("channel.target is not string type");
        return false;
    }
    config->channel.email.target = sdsnew(cJSON_GetStringValue(target));

    return true;
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

    cJSON *load_n_histories = cJSON_GetObjectItem(conf, "load_n_histories");
    if (!load_n_histories || !cJSON_IsNumber(load_n_histories))
    {
        log_error("load_n_histories is not number type");
        goto exit_free_config;
    }
    config->load_n_histories = load_n_histories->valueint;

    cJSON *memory_compress_threshold = cJSON_GetObjectItem(conf, "memory_compress_threshold");
    if (!memory_compress_threshold || !cJSON_IsNumber(memory_compress_threshold))
    {
        config->memory_compress_threshold = 100;
    }
    else
    {
        config->memory_compress_threshold = memory_compress_threshold->valueint;
    }

    cJSON *tool_max_output_size = cJSON_GetObjectItem(conf, "tool_max_output_size");
    if (!tool_max_output_size || !cJSON_IsNumber(tool_max_output_size))
    {
        log_error("tool_max_output_size is not number type");
        goto exit_free_config;
    }
    config->tool_max_output_size = tool_max_output_size->valueint;

    cJSON *device_pty = cJSON_GetObjectItem(conf, "device_pty");
    if (device_pty && cJSON_IsString(device_pty))
        config->device_pty = sdsnew(device_pty->valuestring);

    cJSON *device_ip = cJSON_GetObjectItem(conf, "device_ip");
    if (device_ip && cJSON_IsString(device_ip))
        config->device_ip = sdsnew(device_ip->valuestring);

    cJSON *device_port = cJSON_GetObjectItem(conf, "device_port");
    config->device_port = (device_port && cJSON_IsNumber(device_port)) ? device_port->valueint : 5000;

    cJSON *device_baud = cJSON_GetObjectItem(conf, "device_baud");
    config->device_baud = (device_baud && cJSON_IsNumber(device_baud)) ? device_baud->valueint : 115200;

    cJSON *device_transport = cJSON_GetObjectItem(conf, "device_transport");
    if (device_transport && cJSON_IsString(device_transport))
        config->device_transport = sdsnew(device_transport->valuestring);

    cJSON *channel = cJSON_GetObjectItem(conf, "channel");
    if (!channel || !cJSON_IsObject(channel))
    {
        log_error("channel is not object type");
        goto exit_free_config;
    }

    cJSON *channel_type = cJSON_GetObjectItem(channel, "type");
    if (!channel_type || !cJSON_IsString(channel_type))
    {
        log_error("channel.type is not string type");
        goto exit_free_config;
    }

    const char *type = cJSON_GetStringValue(channel_type);
    if (strcmp(type, "email") == 0)
    {
        config->channel.type = edc_channel_email;
        if (!edc_config_parse_email_channel(config, channel))
        {
            goto exit_free_config;
        }
    }
    else if (strcmp(type, "cli") == 0)
    {
        config->channel.type = edc_channel_cli;
    }

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

    {
        int ro_count = 0;
        if (config->sandbox_ro_paths)
        {
            while (config->sandbox_ro_paths[ro_count])
                ro_count++;
        }
        config->sandbox_ro_paths = realloc(config->sandbox_ro_paths, (ro_count + 2) * sizeof(sds));
        config->sandbox_ro_paths[ro_count] = sdsnew(config->workspace);
        config->sandbox_ro_paths[ro_count + 1] = NULL;
    }

    if (config->sandbox_rw_paths && config->sandbox_rw_paths[0])
    {
        edc_dir_create_with_parents(config->sandbox_rw_paths[0], 0755, NULL);
        char *rp = realpath(config->sandbox_rw_paths[0], NULL);
        config->sandbox_root_real = rp ? sdsnew(rp) : NULL;
        if (rp) free(rp);
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
    sdsfree(config->channel.email.imap_host);
    sdsfree(config->channel.email.smtp_host);
    sdsfree(config->channel.email.user);
    sdsfree(config->channel.email.passwd);
    sdsfree(config->channel.email.target);
    sdsfree(config->device_pty);
    sdsfree(config->device_ip);
    sdsfree(config->device_transport);
    sdsfree(config->workspace);
    sdsfree(config->sandbox_root_real);
    for (int _i = 0; config->sandbox_ro_paths && config->sandbox_ro_paths[_i]; _i++) sdsfree(config->sandbox_ro_paths[_i]);
    free(config->sandbox_ro_paths);
    for (int _i = 0; config->sandbox_rw_paths && config->sandbox_rw_paths[_i]; _i++) sdsfree(config->sandbox_rw_paths[_i]);
    free(config->sandbox_rw_paths);
    free(config);
}
