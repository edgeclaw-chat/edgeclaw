#include "edc_config.h"
#include "cJSON.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"
#include "edc_secret.h"
#include "log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* 通道序列化 (唯一通道 = adapter)：写进 task/message 的 "channel" 字段, 属于落盘格式
 * 兼容。原先住 edc_channel_actor.c —— 那是边界模块, 而 kernel 的 state_serializer 也
 * 要用它, 会引成一条"底层 → 边界"的反向边; 挪到 edc_channel_type 的定义处后方向就正了。 */
cJSON *edc_channel_to_json(void)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "type", "adapter");
    return data;
}

/* Build a NULL-terminated sds array from a comma-separated path list. */
static sds *edc_config_split_paths(const char *csv)
{
    if (!csv || !csv[0])
        return NULL;

    int cap = 8, n = 0;
    sds *out = g_new0(sds, cap + 1);
    const char *p = csv;
    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if (end > start)
        {
            if (n == cap)
            {
                cap *= 2;
                out = realloc(out, (size_t)(cap + 1) * sizeof(sds));
            }
            out[n++] = sdsnewlen(start, (size_t)(end - start));
        }
    }
    out[n] = NULL;
    return out;
}

/* Fresh copy of the default read-only whitelist (EDC_SANDBOX_DEFAULT_RO_PATHS). */
static sds *edc_config_default_ro_paths(void)
{
    return edc_config_split_paths(EDC_SANDBOX_DEFAULT_RO_PATHS);
}

/* Fresh copy of the default writable set: the app workspace, /tmp, /dev and
 * the user's cache directory (~/.cache). The cache is writable so the tools a
 * shell command may run keep working under the sandbox -- package managers and
 * build systems (bazel, pip, cargo, ...) write their caches there. Follows the
 * same shape as edc_config_default_ro_paths(). */
static sds *edc_config_default_rw_paths(const char *workspace)
{
    sds *out = g_new0(sds, 5);
    out[0] = sdsnew(workspace);
    out[1] = sdsnew("/tmp");
    out[2] = sdsnew("/dev");
    sds home = edc_homedir();
    if (home && sdslen(home) > 0)
        out[3] = edc_path_join(home, ".cache", NULL);
    sdsfree(home);
    return out;
}

static bool sds_array_contains(sds *arr, const char *s)
{
    if (!arr)
        return false;
    for (int i = 0; arr[i]; i++)
        if (strcmp(arr[i], s) == 0)
            return true;
    return false;
}

/* Append |s| to a NULL-terminated sds array unless it is already present. */
static void sds_array_add_unique(sds **arrp, const char *s)
{
    if (sds_array_contains(*arrp, s))
        return;
    int n = 0;
    if (*arrp)
        while ((*arrp)[n])
            n++;
    *arrp = realloc(*arrp, (size_t)(n + 2) * sizeof(sds));
    (*arrp)[n] = sdsnew(s);
    (*arrp)[n + 1] = NULL;
}

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
    /* Always positive: a key that is missing, non-numeric or <= 0 (which is what
     * an older config.json may hold) falls back to the default window. The fold
     * has no "unconfigured window" state to fall back to. */
    cJSON *context_window = cJSON_GetObjectItem(conf, "context_window");
    config->context_window = (context_window && cJSON_IsNumber(context_window) && context_window->valueint > 0)
                                 ? context_window->valueint
                                 : EDC_CONFIG_DEFAULT_CONTEXT_WINDOW;

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

    /* channel: 唯一的通道实现是 adapter (TUI)。老配置里的 cli/telegram/... 一律
     * 拒绝。配置里已经没有需要**存下来**的通道字段了 —— 这里只做校验
     * (告警/报错文案与多通道时代一致, 那是既有行为)。 */
    cJSON *channel = cJSON_GetObjectItem(conf, "channel");
    if (!channel || !cJSON_IsObject(channel))
    {
        log_warn("channel config missing, defaulting to 'adapter'");
    }
    else
    {
        cJSON *channel_type = cJSON_GetObjectItem(channel, "type");
        if (!channel_type || !cJSON_IsString(channel_type))
        {
            log_warn("channel.type missing, defaulting to 'adapter'");
        }
        else
        {
            const char *type = cJSON_GetStringValue(channel_type);
            if (strcmp(type, "cli") == 0)
            {
                log_error("channel type 'cli' is no longer supported, set channel.type to \"adapter\"");
                goto exit_free_config;
            }
            else if (strcmp(type, "adapter") != 0)
            {
                log_error("unknown channel type: %s, only 'adapter' is supported", type);
                goto exit_free_config;
            }
        }
    }

    config->workspace = edc_path_join(edc_home, "workspace", NULL);

    /* Sandbox master switch, persisted under "sandbox.enable". It defaults to
     * on when the key is absent (first run / older config files). The CLI flag
     * overrides this for the current run only (see edc_app_new), whereas the
     * in-TUI /sandbox command persists it back to config.json. */
    config->enable_sandbox = true;

    cJSON *sandbox = cJSON_GetObjectItem(conf, "sandbox");
    if (sandbox && cJSON_IsObject(sandbox))
    {
        cJSON *enable = cJSON_GetObjectItem(sandbox, "enable");
        if (enable && cJSON_IsBool(enable))
            config->enable_sandbox = cJSON_IsTrue(enable);

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
            config->sandbox_rw_paths = edc_config_default_rw_paths(config->workspace);
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
            config->sandbox_ro_paths = edc_config_default_ro_paths();
        }

        cJSON *deny = cJSON_GetObjectItem(sandbox, "deny_paths");
        if (deny && cJSON_IsArray(deny) && cJSON_GetArraySize(deny) > 0)
        {
            int size = cJSON_GetArraySize(deny);
            config->sandbox_deny_paths = g_new0(sds, size + 1);
            for (int i = 0; i < size; i++)
                config->sandbox_deny_paths[i] = sdsnew(cJSON_GetStringValue(cJSON_GetArrayItem(deny, i)));
        }
    }
    else
    {
        config->sandbox_rw_paths = edc_config_default_rw_paths(config->workspace);

        config->sandbox_ro_paths = edc_config_default_ro_paths();
    }

    /* The app's own workspace must be writable: the model edits files there
     * (AGENTS.md, skills/, ...). Make sure it is in rw_paths -- which grants
     * read too -- rather than relying on config.json, and skip it when it is
     * already present (the default list includes it). rw_paths[0] stays the
     * launch directory (prepended below), so the sandbox root is unchanged. */
    {
        bool have_workspace = false;
        for (int i = 0; config->sandbox_rw_paths && config->sandbox_rw_paths[i] && !have_workspace; i++)
        {
            char *entry_real = realpath(config->sandbox_rw_paths[i], NULL);
            const char *entry_key = entry_real ? entry_real : config->sandbox_rw_paths[i];
            if (strcmp(config->workspace, entry_key) == 0)
                have_workspace = true;
            if (entry_real)
                free(entry_real);
        }
        if (!have_workspace)
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
    }

    /* The user's home directory is read-only: the model may read the user's
     * files but must not modify them. The workspace and the launch directory
     * are in rw_paths above; path_allowed()'s longest-prefix-wins rule lets
     * those more specific entries win, so they stay writable even though they
     * live inside the read-only home. This also covers ~/.edgeclaw (config.json
     * and the key verifier) without a separate entry. */
    {
        sds home = edc_homedir();
        if (home && sdslen(home) > 0)
        {
            int ro_count = 0;
            if (config->sandbox_ro_paths)
            {
                while (config->sandbox_ro_paths[ro_count])
                    ro_count++;
            }
            config->sandbox_ro_paths = realloc(config->sandbox_ro_paths, (ro_count + 2) * sizeof(sds));
            config->sandbox_ro_paths[ro_count] = home;
            config->sandbox_ro_paths[ro_count + 1] = NULL;
        }
        else
        {
            sdsfree(home);
        }
    }

    /* Migrate the legacy "whole filesystem readable" default: older config.json
     * files carry "/" in ro_paths, which makes Landlock grant READ to the
     * entire filesystem. Strip it and top up the system whitelist so reads are
     * confined to that whitelist (plus the read-only home appended above). A
     * user who really wants the whole root readable can re-add it with
     * `/sandbox add-ro /`. */
    if (config->sandbox_ro_paths)
    {
        bool removed_root = false;
        int n = 0;
        for (int i = 0; config->sandbox_ro_paths[i]; i++)
        {
            if (strcmp(config->sandbox_ro_paths[i], "/") == 0)
            {
                sdsfree(config->sandbox_ro_paths[i]);
                removed_root = true;
                continue;
            }
            config->sandbox_ro_paths[n++] = config->sandbox_ro_paths[i];
        }
        config->sandbox_ro_paths[n] = NULL;

        if (removed_root)
        {
            sds *defs = edc_config_default_ro_paths();
            for (int i = 0; defs && defs[i]; i++)
            {
                sds_array_add_unique(&config->sandbox_ro_paths, defs[i]);
                sdsfree(defs[i]);
            }
            free(defs);
            log_info(
                "edc_config: sandbox ro_paths: dropped legacy '/' (whole-filesystem read), using system whitelist");
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
    /* Free through the real destructor: by the time any of the goto
     * exit_free_config sites fire, fields such as edc_home (and possibly
     * model/api_url) are already allocated. A bare free(config) leaked them.
     * edc_config_free() is NULL-safe for the not-yet-filled fields. */
    edc_config_free(config);
exit_free_conf:
    cJSON_Delete(conf);
exit_free_content:
    sdsfree(content);
exit_free_path:
    sdsfree(path);

    return NULL;
}

/* Grant read+write inside the filesystem sandbox to |path| by prepending it to
 * sandbox_rw_paths. Used when a session binds a working directory: the bound
 * directory must be admissible under the kernel sandbox (the helper receives
 * sandbox_rw_paths as its --rw set) and under the in-process path validation
 * (edc_sandbox_verify_path), exactly like the launch directory is.
 *
 * Deduplicated by realpath, so binding the launch directory (or a directory
 * already in the set) is a no-op. Prepending keeps the most recently bound
 * directory ahead of broader entries; path_allowed() ultimately decides by
 * longest-prefix match, so order alone never widens access -- a read-only
 * subtree still wins over a shorter writable parent. Returns true when the
 * path is now in the set (or was already), false on a NULL/empty path or a
 * path that cannot be resolved. */
bool edc_config_add_sandbox_rw(struct edc_config *conf, const char *path)
{
    if (!conf || !path || !path[0])
        return false;

    char *resolved = realpath(path, NULL);
    if (!resolved)
        return false;

    bool duplicate = false;
    for (int i = 0; conf->sandbox_rw_paths && conf->sandbox_rw_paths[i] && !duplicate; i++)
    {
        char *entry_real = realpath(conf->sandbox_rw_paths[i], NULL);
        const char *entry_key = entry_real ? entry_real : conf->sandbox_rw_paths[i];
        if (strcmp(resolved, entry_key) == 0)
            duplicate = true;
        if (entry_real)
            free(entry_real);
    }

    if (!duplicate)
    {
        int n = 0;
        while (conf->sandbox_rw_paths && conf->sandbox_rw_paths[n])
            n++;
        conf->sandbox_rw_paths = realloc(conf->sandbox_rw_paths, (size_t)(n + 2) * sizeof(sds));
        for (int i = n; i > 0; i--)
            conf->sandbox_rw_paths[i] = conf->sandbox_rw_paths[i - 1];
        conf->sandbox_rw_paths[0] = sdsnew(resolved);
        conf->sandbox_rw_paths[n + 1] = NULL;
    }

    free(resolved);
    return true;
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
    for (int _i = 0; config->sandbox_deny_paths && config->sandbox_deny_paths[_i]; _i++)
        sdsfree(config->sandbox_deny_paths[_i]);
    free(config->sandbox_deny_paths);
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

/* Persist the sandbox master switch to <edc_home>/config.json, under
 * "sandbox.enable". Merges into the existing file so rw_paths/ro_paths and all
 * other settings are preserved. Used by the in-TUI /sandbox command; the
 * command-line flag deliberately does NOT go through here (it is a per-run
 * override only). Returns true on success. */
bool edc_config_update_sandbox_enabled(const char *edc_home, bool enable)
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

    /* Reuse the existing "sandbox" object when present so its rw_paths /
     * ro_paths survive; otherwise create it (edc_init_env normally writes it). */
    cJSON *sandbox = cJSON_GetObjectItem(conf, "sandbox");
    if (!sandbox || !cJSON_IsObject(sandbox))
    {
        cJSON_DeleteItemFromObject(conf, "sandbox");
        sandbox = cJSON_AddObjectToObject(conf, "sandbox");
    }
    cJSON_DeleteItemFromObject(sandbox, "enable");
    cJSON_AddBoolToObject(sandbox, "enable", enable);

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

/* Index of the first string element equal to |s| in a cJSON array, or -1. */
static int cjson_array_index_of(cJSON *arr, const char *s)
{
    if (!arr || !cJSON_IsArray(arr))
        return -1;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++)
    {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        if (e && cJSON_IsString(e) && strcmp(e->valuestring, s) == 0)
            return i;
    }
    return -1;
}

/* Return the "sandbox"."<key>" array, creating an empty one if missing. */
static cJSON *cjson_ensure_array(cJSON *sandbox, const char *key)
{
    cJSON *arr = cJSON_GetObjectItem(sandbox, key);
    if (arr && cJSON_IsArray(arr))
        return arr;
    cJSON_DeleteItemFromObject(sandbox, key);
    arr = cJSON_CreateArray();
    cJSON_AddItemToObject(sandbox, key, arr);
    return arr;
}

bool edc_config_update_sandbox_path(const char *edc_home, const char *path, edc_sandbox_path_kind kind, bool add)
{
    if (!edc_home || !path || path[0] != '/')
        return false;

    sds conf_path = edc_path_join(edc_home, "config.json", NULL);
    EdcError *error = NULL;
    sds contents = edc_file_read(conf_path, &error);
    if (!contents)
    {
        log_error("edc_config: cannot read %s: %s", conf_path, error ? error->message : "unknown");
        if (error)
            edc_error_free(error);
        sdsfree(conf_path);
        return false;
    }

    cJSON *conf = cJSON_Parse(contents);
    sdsfree(contents);
    if (!conf || !cJSON_IsObject(conf))
    {
        if (conf)
            cJSON_Delete(conf);
        sdsfree(conf_path);
        return false;
    }

    cJSON *sandbox = cJSON_GetObjectItem(conf, "sandbox");
    if (!sandbox || !cJSON_IsObject(sandbox))
    {
        cJSON_DeleteItemFromObject(conf, "sandbox");
        sandbox = cJSON_AddObjectToObject(conf, "sandbox");
    }

    /* Normalize a legacy whole-filesystem ro_paths entry so an edit can never
     * leave "/" behind: strip it, and top up the whitelist if that emptied it. */
    {
        cJSON *roarr = cJSON_GetObjectItem(sandbox, "ro_paths");
        if (roarr && cJSON_IsArray(roarr))
        {
            int i;
            while ((i = cjson_array_index_of(roarr, "/")) >= 0)
                cJSON_DeleteItemFromArray(roarr, i);
            if (cJSON_GetArraySize(roarr) == 0)
            {
                sds *defs = edc_config_default_ro_paths();
                for (int k = 0; defs && defs[k]; k++)
                {
                    cJSON_AddItemToArray(roarr, cJSON_CreateString(defs[k]));
                    sdsfree(defs[k]);
                }
                free(defs);
            }
        }
    }

    static const char *const lists[3] = {"ro_paths", "rw_paths", "deny_paths"};
    const char *key = lists[(int)kind];
    cJSON *arr = cjson_ensure_array(sandbox, key);

    if (add)
    {
        /* Keep a path in exactly one list: adding it here drops it from the
         * other two, so an explicit allow overrides a previous deny and vice
         * versa. (A deny that is a *sub-directory* of an allowed parent is left
         * in place -- that is exactly the mask use case.) */
        for (int i = 0; i < 3; i++)
        {
            if (strcmp(lists[i], key) == 0)
                continue;
            cJSON *other = cjson_ensure_array(sandbox, lists[i]);
            int oi = cjson_array_index_of(other, path);
            if (oi >= 0)
                cJSON_DeleteItemFromArray(other, oi);
        }
        if (cjson_array_index_of(arr, path) < 0)
            cJSON_AddItemToArray(arr, cJSON_CreateString(path));
    }
    else
    {
        int i = cjson_array_index_of(arr, path);
        if (i >= 0)
            cJSON_DeleteItemFromArray(arr, i);
    }

    char *out = cJSON_Print(conf);
    cJSON_Delete(conf);
    if (!out)
    {
        sdsfree(conf_path);
        return false;
    }

    bool ok = edc_file_write(conf_path, out, -1, &error);
    if (!ok)
    {
        log_error("edc_config: cannot write %s: %s", conf_path, error ? error->message : "unknown");
        if (error)
            edc_error_free(error);
    }
    cJSON_free(out);
    sdsfree(conf_path);
    return ok;
}

bool edc_config_set_sandbox_enabled(struct edc_config *cfg, bool enable)
{
    if (!cfg)
        return false;

    /* Persist first (mirrors edc_app_set_llm_config): if config.json cannot
     * be written, leave the running app untouched so memory and disk agree. */
    if (!edc_config_update_sandbox_enabled(cfg->edc_home, enable))
        return false;

    cfg->enable_sandbox = enable;
    return true;
}

/* ── sandbox 配置的写口 ──────────────────────────────────────
 * 先落盘 (config.json), 成功后才改内存 —— 失败时内存与磁盘保持一致。
 * 它们只碰 config, 所以归 config 模块 (形状 C: 一个概念一个 owner)。 */

static bool sds_path_list_contains(sds *arr, const char *p)
{
    if (!arr)
        return false;
    for (int i = 0; arr[i]; i++)
        if (strcmp(arr[i], p) == 0)
            return true;
    return false;
}

static void sds_path_list_add(sds **arrp, const char *p)
{
    if (sds_path_list_contains(*arrp, p))
        return;
    int n = 0;
    if (*arrp)
        while ((*arrp)[n])
            n++;
    *arrp = realloc(*arrp, (size_t)(n + 2) * sizeof(sds));
    (*arrp)[n] = sdsnew(p);
    (*arrp)[n + 1] = NULL;
}

static void sds_path_list_remove(sds **arrp, const char *p)
{
    if (!arrp || !*arrp)
        return;
    int n = 0;
    for (int i = 0; (*arrp)[i]; i++)
    {
        if (strcmp((*arrp)[i], p) == 0)
        {
            sdsfree((*arrp)[i]);
            continue;
        }
        (*arrp)[n++] = (*arrp)[i];
    }
    (*arrp)[n] = NULL;
}

bool edc_config_set_sandbox_path(struct edc_config *cfg, const char *path, edc_sandbox_path_kind kind, bool add,
                                 sds *msg)
{
    if (msg)
        *msg = NULL;
    if (!cfg)
    {
        if (msg)
            *msg = sdsnew("config not ready");
        return false;
    }
    if (!path || path[0] != '/')
    {
        if (msg)
            *msg = sdsnew("path must be absolute");
        return false;
    }

    char *resolved = realpath(path, NULL);
    if (!resolved)
    {
        if (msg)
            *msg = sdscatprintf(sdsempty(), "no such directory: %s", path);
        return false;
    }
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        if (msg)
            *msg = sdscatprintf(sdsempty(), "not a directory: %s", resolved);
        free(resolved);
        return false;
    }

    /* Persist first (mirrors edc_app_set_sandbox_enabled): on failure leave
     * the running config untouched so memory and disk agree. */
    if (!edc_config_update_sandbox_path(cfg->edc_home, resolved, kind, add))
    {
        if (msg)
            *msg = sdsnew("failed to write config.json");
        free(resolved);
        return false;
    }

    /* A path lives in exactly one list; mirror the persist step in memory. */
    sds **ro = &cfg->sandbox_ro_paths;
    sds **rw = &cfg->sandbox_rw_paths;
    sds **dn = &cfg->sandbox_deny_paths;
    sds **target = (kind == EDC_SANDBOX_PATH_RW) ? rw : (kind == EDC_SANDBOX_PATH_DENY) ? dn : ro;

    if (add)
    {
        sds_path_list_add(target, resolved);
        if (target != ro)
            sds_path_list_remove(ro, resolved);
        if (target != rw)
            sds_path_list_remove(rw, resolved);
        if (target != dn)
            sds_path_list_remove(dn, resolved);
    }
    else
    {
        sds_path_list_remove(target, resolved);
    }

    free(resolved);
    return true;
}
