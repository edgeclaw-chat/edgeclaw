#include "edc_config.h"
#include "edc_secret.h"
#include "cJSON.h"
#include "log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

gchar *edc_config_resolve_bc_home(void)
{
    const gchar *home = g_get_home_dir();
    if (!home)
    {
        log_error("g_get_home_dir() returned NULL, falling back to /tmp/.edgeclaw");
        home = "/tmp";
    }
    return g_build_filename(home, ".edgeclaw", NULL);
}

static void secure_free(gchar *str)
{
    if (str)
    {
        memset(str, 0, strlen(str));
        g_free(str);
    }
}

static gboolean edc_config_parse_email_channel(struct edc_config *config, cJSON *channel)
{
    cJSON *imap_host = cJSON_GetObjectItem(channel, "imap_host");
    if (!imap_host || !cJSON_IsString(imap_host))
    {
        log_error("channel.imap_host is not string type");
        return FALSE;
    }

    config->channel.email.imap_host = g_strdup(cJSON_GetStringValue(imap_host));

    cJSON *smtp_host = cJSON_GetObjectItem(channel, "smtp_host");
    if (!smtp_host || !cJSON_IsString(smtp_host))
    {
        log_error("channel.smtp_host is not string type");
        return FALSE;
    }
    config->channel.email.smtp_host = g_strdup(cJSON_GetStringValue(smtp_host));

    cJSON *user = cJSON_GetObjectItem(channel, "user");
    if (!user || !cJSON_IsString(user))
    {
        log_error("channel.user is not string type");
        return FALSE;
    }
    config->channel.email.user = g_strdup(cJSON_GetStringValue(user));

    cJSON *password = cJSON_GetObjectItem(channel, "passwd");
    if (!password || !cJSON_IsString(password))
    {
        log_error("channel.passwd is not string type");
        return FALSE;
    }
    config->channel.email.passwd = edc_secret_decrypt(cJSON_GetStringValue(password));

    cJSON *target = cJSON_GetObjectItem(channel, "target");
    if (!target || !cJSON_IsString(target))
    {
        log_error("channel.target is not string type");
        return FALSE;
    }
    config->channel.email.target = g_strdup(cJSON_GetStringValue(target));

    return TRUE;
}

static gboolean edc_config_parse_telegram_channel(struct edc_config *config, cJSON *channel)
{
    cJSON *bot_key = cJSON_GetObjectItem(channel, "bot_key");
    cJSON *user_id = cJSON_GetObjectItem(channel, "user_id");
    if (!bot_key || !cJSON_IsString(bot_key))
    {
        gchar *s = cJSON_PrintUnformatted(channel);
        log_error("channel.bot_key is not string type: %s", s);
        g_free(s);
        return FALSE;
    }

    if (!user_id || !cJSON_IsNumber(user_id))
    {
        log_error("channel.user_id is not string type: %s", cJSON_PrintUnformatted(channel));
        return FALSE;
    }

    config->channel.telegram.bot_key = edc_secret_decrypt(cJSON_GetStringValue(bot_key));
    config->channel.telegram.user_id = (gint64)cJSON_GetNumberValue(user_id);
    return TRUE;
}

static gboolean edc_config_parse_feishu_channel(struct edc_config *config, cJSON *channel)
{
    cJSON *app_id = cJSON_GetObjectItem(channel, "app_id");
    if (!app_id || !cJSON_IsString(app_id))
    {
        log_error("channel.app_id is not string type");
        return FALSE;
    }
    config->channel.feishu.app_id = g_strdup(cJSON_GetStringValue(app_id));

    cJSON *app_secret = cJSON_GetObjectItem(channel, "app_secret");
    if (!app_secret || !cJSON_IsString(app_secret))
    {
        log_error("channel.app_secret is not string type");
        return FALSE;
    }
    config->channel.feishu.app_secret = edc_secret_decrypt(cJSON_GetStringValue(app_secret));

    return TRUE;
}

static gboolean edc_config_parse_qq_channel(struct edc_config *config, cJSON *channel)
{
    cJSON *app_id = cJSON_GetObjectItem(channel, "app_id");
    if (!app_id || !cJSON_IsString(app_id))
    {
        log_error("channel.app_id is not string type");
        return FALSE;
    }
    config->channel.qq.app_id = g_strdup(cJSON_GetStringValue(app_id));

    cJSON *app_secret = cJSON_GetObjectItem(channel, "app_secret");
    if (!app_secret || !cJSON_IsString(app_secret))
    {
        log_error("channel.app_secret is not string type");
        return FALSE;
    }
    config->channel.qq.app_secret = edc_secret_decrypt(cJSON_GetStringValue(app_secret));

    return TRUE;
}

struct edc_config *edc_config_load(const gchar *edc_home)
{
    gchar *content = NULL;
    gsize len = 0;
    GError *error = NULL;
    struct edc_config *config = NULL;

    gchar *path = g_build_filename(edc_home, "config.json", NULL);
    gboolean success = g_file_get_contents(path, &content, &len, &error);

    if (!success)
    {
        log_error("failed to load config file: %s", error->message);
        g_error_free(error);
        goto exit_free_path;
    }

    cJSON *conf = cJSON_ParseWithLength(content, len);
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

    config->edc_home = g_strdup(edc_home);

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
        config->llm_api_conf.api_key = edc_secret_decrypt(cJSON_GetStringValue(api_key));
    }

    cJSON *model = cJSON_GetObjectItem(conf, "model");
    if (!model || !cJSON_IsString(model))
    {
        log_error("model is not string type");
        goto exit_free_config;
    }
    config->llm_api_conf.model = g_strdup(cJSON_GetStringValue(model));

    cJSON *api_url = cJSON_GetObjectItem(conf, "api_url");
    if (!api_url || !cJSON_IsString(api_url))
    {
        log_error("api_url is not string type");
        goto exit_free_config;
    }
    config->llm_api_conf.api_url = g_strdup(cJSON_GetStringValue(api_url));

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

    cJSON *tool_max_output_size = cJSON_GetObjectItem(conf, "tool_max_output_size");
    if (!tool_max_output_size || !cJSON_IsNumber(tool_max_output_size))
    {
        log_error("tool_max_output_size is not number type");
        goto exit_free_config;
    }
    config->tool_max_output_size = tool_max_output_size->valueint;

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

    const gchar *type = cJSON_GetStringValue(channel_type);
    if (strcmp(type, "email") == 0)
    {
        config->channel.type = edc_channel_email;
        if (!edc_config_parse_email_channel(config, channel))
        {
            goto exit_free_config;
        }
    }
    else if (strcmp(type, "telegram") == 0)
    {
        config->channel.type = edc_channel_telegram;
        if (!edc_config_parse_telegram_channel(config, channel))
        {
            goto exit_free_config;
        }
    }
    else if (strcmp(type, "feishu") == 0)
    {
        config->channel.type = edc_channel_feishu;
        if (!edc_config_parse_feishu_channel(config, channel))
        {
            goto exit_free_config;
        }
    }
    else if (strcmp(type, "qq") == 0)
    {
        config->channel.type = edc_channel_qq;
        if (!edc_config_parse_qq_channel(config, channel))
        {
            goto exit_free_config;
        }
    }
    else if (strcmp(type, "cli") == 0)
    {
        config->channel.type = edc_channel_cli;
    }

    config->workspace = g_build_filename(edc_home, "workspace", NULL);

    cJSON *sandbox = cJSON_GetObjectItem(conf, "sandbox");
    if (sandbox && cJSON_IsObject(sandbox))
    {
        cJSON *rw = cJSON_GetObjectItem(sandbox, "rw_paths");
        if (rw && cJSON_IsArray(rw) && cJSON_GetArraySize(rw) > 0)
        {
            int size = cJSON_GetArraySize(rw);
            config->sandbox_rw_paths = g_new0(gchar *, size + 1);
            for (int i = 0; i < size; i++)
                config->sandbox_rw_paths[i] = g_strdup(cJSON_GetStringValue(cJSON_GetArrayItem(rw, i)));
        }
        else
        {
            config->sandbox_rw_paths = g_new0(gchar *, 3 + 1);
            config->sandbox_rw_paths[0] = g_build_filename(edc_home, "sandbox", NULL);
            config->sandbox_rw_paths[1] = g_strdup("/tmp");
            config->sandbox_rw_paths[2] = g_strdup("/dev");
        }

        cJSON *ro = cJSON_GetObjectItem(sandbox, "ro_paths");
        if (ro && cJSON_IsArray(ro) && cJSON_GetArraySize(ro) > 0)
        {
            int size = cJSON_GetArraySize(ro);
            config->sandbox_ro_paths = g_new0(gchar *, size + 1);
            for (int i = 0; i < size; i++)
                config->sandbox_ro_paths[i] = g_strdup(cJSON_GetStringValue(cJSON_GetArrayItem(ro, i)));
        }
        else
        {
            config->sandbox_ro_paths = g_new0(gchar *, 10 + 1);
            config->sandbox_ro_paths[0] = g_strdup("/");
            config->sandbox_ro_paths[1] = g_strdup("/usr");
            config->sandbox_ro_paths[2] = g_strdup("/lib");
            config->sandbox_ro_paths[3] = g_strdup("/lib32");
            config->sandbox_ro_paths[4] = g_strdup("/lib64");
            config->sandbox_ro_paths[5] = g_strdup("/libx32");
            config->sandbox_ro_paths[6] = g_strdup("/bin");
            config->sandbox_ro_paths[7] = g_strdup("/sbin");
            config->sandbox_ro_paths[8] = g_strdup("/etc");
            config->sandbox_ro_paths[9] = g_strdup("/proc");
        }
    }
    else
    {
        config->sandbox_rw_paths = g_new0(gchar *, 3 + 1);
        config->sandbox_rw_paths[0] = g_build_filename(edc_home, "sandbox", NULL);
        config->sandbox_rw_paths[1] = g_strdup("/tmp");
        config->sandbox_rw_paths[2] = g_strdup("/dev");

        config->sandbox_ro_paths = g_new0(gchar *, 10 + 1);
        config->sandbox_ro_paths[0] = g_strdup("/");
        config->sandbox_ro_paths[1] = g_strdup("/usr");
        config->sandbox_ro_paths[2] = g_strdup("/lib");
        config->sandbox_ro_paths[3] = g_strdup("/lib32");
        config->sandbox_ro_paths[4] = g_strdup("/lib64");
        config->sandbox_ro_paths[5] = g_strdup("/libx32");
        config->sandbox_ro_paths[6] = g_strdup("/bin");
        config->sandbox_ro_paths[7] = g_strdup("/sbin");
        config->sandbox_ro_paths[8] = g_strdup("/etc");
        config->sandbox_ro_paths[9] = g_strdup("/proc");
    }

    {
        int ro_count = 0;
        if (config->sandbox_ro_paths)
        {
            while (config->sandbox_ro_paths[ro_count])
                ro_count++;
        }
        config->sandbox_ro_paths = g_renew(gchar *, config->sandbox_ro_paths, ro_count + 2);
        config->sandbox_ro_paths[ro_count] = g_strdup(config->workspace);
        config->sandbox_ro_paths[ro_count + 1] = NULL;
    }

    if (config->sandbox_rw_paths && config->sandbox_rw_paths[0])
    {
        g_mkdir_with_parents(config->sandbox_rw_paths[0], 0755);
        config->sandbox_root_real = realpath(config->sandbox_rw_paths[0], NULL);
        if (!config->sandbox_root_real)
        {
            log_error("edc_config: cannot resolve sandbox root '%s': %s", config->sandbox_rw_paths[0], strerror(errno));
        }
    }

    cJSON_Delete(conf);
    g_free(content);
    g_free(path);

    return config;

exit_free_config:
    g_free(config);
exit_free_conf:
    cJSON_Delete(conf);
exit_free_content:
    g_free(content);
exit_free_path:
    g_free(path);

    return NULL;
}

void edc_config_free(struct edc_config *config)
{
    if (!config)
        return;

    g_free(config->llm_api_conf.model);
    secure_free(config->llm_api_conf.api_key);
    g_free(config->llm_api_conf.api_url);
    g_free(config->edc_home);
    g_free(config->channel.email.imap_host);
    g_free(config->channel.email.smtp_host);
    g_free(config->channel.email.user);
    secure_free(config->channel.email.passwd);
    g_free(config->channel.email.target);
    g_free(config->workspace);
    g_free(config->sandbox_root_real);
    g_strfreev(config->sandbox_ro_paths);
    g_strfreev(config->sandbox_rw_paths);
    secure_free(config->channel.telegram.bot_key);
    g_free(config->channel.feishu.app_id);
    secure_free(config->channel.feishu.app_secret);
    g_free(config->channel.qq.app_id);
    secure_free(config->channel.qq.app_secret);
    g_free(config);
}
