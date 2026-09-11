#include "edc_sandbox.h"
#include "edc_bpf_sandbox.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_landlock_sandbox.h"
#include "edc_ns_sandbox.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "edc_error.h"

bool edc_sandbox_init(struct edc_config *conf)
{
    if (!conf->sandbox_root_real)
    {
        log_error("edc_sandbox: sandbox root could not be resolved");
        return false;
    }

#ifdef __linux__
    if (conf->enable_sandbox)
    {
        /* Linux 上有 Landlock/NS/seccomp-BPF 三个内核隔离后端, 但受多线程
         * fork 安全性限制尚未接线 (见 edc_sandbox_apply 注释)。当前生效的
         * 是 fs/shell/device 工具层的 sandbox_root 路径校验。 */
        log_info("edc_sandbox: kernel isolation backends present but not wired; "
                 "path validation is the active enforcement");
    }
#endif

    log_info("edc_sandbox: initialized, root = %s", conf->sandbox_root_real);
    return true;
}

const char *edc_sandbox_get_root(struct edc_config *conf)
{
    return conf->sandbox_root_real;
}

/* Length of the longest real-path prefix in `paths` that contains
 * `resolved_path` (the match must end at a path boundary), or -1 when no entry
 * matches. */
static int path_prefix_match_len(const char *resolved_path, char **paths)
{
    if (!paths || !resolved_path)
        return -1;

    int best = -1;
    for (int i = 0; paths[i]; i++)
    {
        char *entry_real = realpath(paths[i], NULL);
        if (!entry_real)
            continue;

        size_t len = strlen(entry_real);
        if (strncmp(resolved_path, entry_real, len) == 0 &&
            (resolved_path[len] == '\0' || resolved_path[len] == '/'))
        {
            if ((int)len > best)
                best = (int)len;
        }
        free(entry_real);
    }

    return best;
}

static bool path_allowed(struct edc_config *conf, const char *resolved_path, bool writable)
{
    if (!conf->sandbox_root_real)
        return true;

    int rw_len = path_prefix_match_len(resolved_path, conf->sandbox_rw_paths);
    int ro_len = path_prefix_match_len(resolved_path, conf->sandbox_ro_paths);

    /* Reads are granted by either set (rw also grants read). */
    if (!writable)
        return rw_len >= 0 || ro_len >= 0;

    /* Writes are decided by the most specific (longest) matching entry, so an
     * explicit read-write path carves a writable hole inside a read-only
     * subtree, and a deeper read-only entry keeps that subtree read-only even
     * when its parent is writable. A tie (the same directory in both sets)
     * resolves to read-only -- deny wins -- so e.g. launching from the home
     * directory does not silently make the whole home writable. */
    return rw_len >= 0 && rw_len > ro_len;
}

sds edc_sandbox_verify_path(struct edc_config *conf, const char *path, bool writable, EdcError **error)
{
    if (!conf->enable_sandbox || !conf->sandbox_root_real)
    {
        return sdsnew(path);
    }

    if (!path || !path[0])
    {
        if (error)
            *error = edc_error_new(EDC_ERR_SANDBOX, "empty path");
        return NULL;
    }

    char *resolved = realpath(path, NULL);
    if (resolved)
    {
        if (path_allowed(conf, resolved, writable))
        {
            sds ret = sdsnew(resolved);
            free(resolved);
            return ret;
        }

        free(resolved);
        if (error)
            *error = edc_error_new(EDC_ERR_SANDBOX, "access denied: '%s' is outside sandbox", path);
        return NULL;
    }

    sds parent = edc_path_dirname(path);
    char *parent_real = realpath(parent, NULL);
    sdsfree(parent);

    if (!parent_real)
    {
        if (error)
            *error = edc_error_new(EDC_ERR_SANDBOX, "cannot resolve path: %s", path);
        return NULL;
    }

    if (!path_allowed(conf, parent_real, writable))
    {
        free(parent_real);
        if (error)
            *error = edc_error_new(EDC_ERR_SANDBOX, "access denied: '%s' is outside sandbox", path);
        return NULL;
    }

    const char *last_slash = strrchr(path, '/');
    sds result = edc_path_join(parent_real, last_slash ? last_slash + 1 : path, NULL);
    free(parent_real);
    return result;
}

/*
 * Apply kernel sandbox layers to the *current process*. This is meant to run
 * in a child process right after fork() and before exec() — it must never run
 * in the main EdgeClaw process (it would sandbox the agent itself).
 *
 * ⚠️ Wiring decision (2026-09): NOT yet called from the tool layer. exec_shell
 * spawns /bin/sh through uv_spawn on the shared actor-runner thread while the
 * HTTPS worker thread is alive; the backends below (logging plus sds/calloc
 * allocation) are not async-signal-safe, so running them post-fork in a
 * multi-threaded process risks allocator deadlock. uv_spawn also offers no
 * pre-exec hook. Safe wiring paths for later:
 *   1. Harden backends to allocation-free/log-free operation and switch
 *      exec_shell to fork()+sandbox_apply()+exec on Linux, or
 *   2. Spawn a dedicated single-threaded helper process (re-exec self with a
 *      hidden flag) that applies the sandbox then execs the command.
 * Until then, sandbox_root path verification in the fs/shell/device tools is
 * the active enforcement layer on every platform.
 */
bool edc_sandbox_apply(struct edc_config *conf)
{
#ifdef __linux__
    if (!conf->enable_sandbox || !conf->sandbox_root_real)
    {
        log_info("edc_sandbox: sandbox not enabled, not applied");
        return true;
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

    bool fs_ok = false;

    if (edc_landlock_sandbox_apply((const char **)conf->sandbox_ro_paths, (const char **)conf->sandbox_rw_paths))
    {
        fs_ok = true;
    }
    else
    {
        log_info("edc_sandbox: Landlock not available, trying namespace sandbox...");
        if (edc_ns_sandbox_apply((const char **)conf->sandbox_ro_paths, (const char **)conf->sandbox_rw_paths))
        {
            fs_ok = true;
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
        return false;
    }

    return true;
#else
    return true;
#endif
}
