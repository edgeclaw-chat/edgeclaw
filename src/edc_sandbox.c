#include "edc_strutil.h"
#include "edc_sandbox.h"
#include "edc_bpf_sandbox.h"
#include "edc_config.h"
#include "edc_landlock_sandbox.h"
#include "edc_ns_sandbox.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

GQuark edc_sandbox_error_quark(void)
{
    return g_quark_from_static_string("edc_sandbox_error");
}

bool edc_sandbox_init(struct edc_config *conf)
{
    if (!conf->sandbox_root_real)
    {
        log_error("edc_sandbox: sandbox root could not be resolved");
        return FALSE;
    }

    log_info("edc_sandbox: initialized, root = %s", conf->sandbox_root_real);
    return TRUE;
}

const char *edc_sandbox_get_root(struct edc_config *conf)
{
    return conf->sandbox_root_real;
}

static bool path_is_under_any(const char *resolved_path, char **paths)
{
    if (!paths || !resolved_path)
        return FALSE;

    for (int i = 0; paths[i]; i++)
    {
        char *entry_real = realpath(paths[i], NULL);
        if (!entry_real)
            continue;

        size_t len = strlen(entry_real);
        if (edc_str_has_prefix(resolved_path, entry_real) && (resolved_path[len] == '\0' || resolved_path[len] == '/'))
        {
            free(entry_real);
            return TRUE;
        }
        free(entry_real);
    }

    return FALSE;
}

static bool path_allowed(struct edc_config *conf, const char *resolved_path, bool writable)
{
    if (!conf->sandbox_root_real)
        return TRUE;

    if (path_is_under_any(resolved_path, conf->sandbox_rw_paths))
        return TRUE;

    if (!writable && path_is_under_any(resolved_path, conf->sandbox_ro_paths))
        return TRUE;

    return FALSE;
}

char *edc_sandbox_verify_path(struct edc_config *conf, const char *path, bool writable, GError **error)
{
    if (!conf->enable_sandbox || !conf->sandbox_root_real)
    {
        return strdup(path);
    }

    if (!path || !path[0])
    {
        if (error)
            *error = g_error_new(BC_SANDBOX_ERROR, BC_SANDBOX_ERROR_ESCAPE, "empty path");
        return NULL;
    }

    char *resolved = realpath(path, NULL);
    if (resolved)
    {
        if (path_allowed(conf, resolved, writable))
            return resolved;

        free(resolved);
        if (error)
            *error =
                g_error_new(BC_SANDBOX_ERROR, BC_SANDBOX_ERROR_ESCAPE, "access denied: '%s' is outside sandbox", path);
        return NULL;
    }

    char *parent = g_path_get_dirname(path);
    char *parent_real = realpath(parent, NULL);
    free(parent);

    if (!parent_real)
    {
        if (error)
            *error = g_error_new(BC_SANDBOX_ERROR, BC_SANDBOX_ERROR_ESCAPE, "cannot resolve path: %s", path);
        return NULL;
    }

    if (!path_allowed(conf, parent_real, writable))
    {
        free(parent_real);
        if (error)
            *error =
                g_error_new(BC_SANDBOX_ERROR, BC_SANDBOX_ERROR_ESCAPE, "access denied: '%s' is outside sandbox", path);
        return NULL;
    }

    char *basename = g_path_get_basename(path);
    char *result = g_build_filename(parent_real, basename, NULL);
    free(basename);
    free(parent_real);
    return result;
}

bool edc_sandbox_apply(struct edc_config *conf)
{
#ifdef __linux__
    if (!conf->enable_sandbox || !conf->sandbox_root_real)
    {
        log_info("edc_sandbox: sandbox not enabled, not applied");
        return TRUE;
    }

    /*
     * Apply sandbox layers:
     *   1. Landlock (Linux 5.13+) — filesystem isolation
     *   2. Namespace (Linux 3.8+) — fallback filesystem isolation via user+mount namespace
     *   3. seccomp-bpf (always) — block dangerous syscalls (ptrace, mount, etc.)
     *
     * Apply BPF last so that the filesystem sandbox can use mount/unshare/etc.
     * syscalls during its setup. Once the BPF filter is installed, those syscalls
     * are locked down.
     */

    bool fs_ok = FALSE;

    if (edc_landlock_sandbox_apply((const char **)conf->sandbox_ro_paths, (const char **)conf->sandbox_rw_paths))
    {
        fs_ok = TRUE;
    }
    else
    {
        log_info("edc_sandbox: Landlock not available, trying namespace sandbox...");
        if (edc_ns_sandbox_apply((const char **)conf->sandbox_ro_paths, (const char **)conf->sandbox_rw_paths))
        {
            fs_ok = TRUE;
        }
    }

    bool bpfs_ok = edc_bpf_sandbox_apply(conf->sandbox_root_real);
    if (!bpfs_ok)
    {
        log_info("edc_sandbox: seccomp-bpf not available, continuing without syscall filter");
    }

    if (!fs_ok && !bpfs_ok)
    {
        log_warn("edc_sandbox: no kernel sandbox available; relying on path validation only");
        return FALSE;
    }

    return TRUE;
#else
    return TRUE;
#endif
}
