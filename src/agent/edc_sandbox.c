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
#include <sys/stat.h>

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
        /* Kernel isolation is applied per-command inside the embedded sandbox
         * helper (see src/agent/sandbox_helper.c), NOT in the agent process:
         * the layers are irreversible and inherited, so applying them here
         * would sandbox the agent itself. In-process enforcement is the
         * tool-layer path verification in edc_sandbox_verify_path().
         * edc_sandbox_apply() below is reserved for a confined single-threaded
         * child and is currently unused. */
        log_info("edc_sandbox: kernel isolation runs in the per-command helper; "
                 "path validation is the in-process enforcement");
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
        if (strncmp(resolved_path, entry_real, len) == 0 && (resolved_path[len] == '\0' || resolved_path[len] == '/'))
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

    /* Deny wins over every allow entry, for reads and writes alike. This covers
     * the file tools (which run in the agent process and have no kernel
     * sandbox); exec_shell is additionally masked at the mount level. */
    if (path_prefix_match_len(resolved_path, conf->sandbox_deny_paths) >= 0)
        return false;

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

    /* Never write through a symlink. A dangling target makes realpath() fail,
     * and the fallback below would then return the symlink path itself, so a
     * planted link (e.g. workspace/x -> ~/.edgeclaw/config.json) could redirect
     * the write into a read-only location. Reads may follow symlinks. */
    if (writable)
    {
        struct stat lst;
        if (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode))
        {
            if (error)
                *error = edc_error_new(EDC_ERR_SANDBOX, "access denied: refusing to write through symlink '%s'", path);
            return NULL;
        }
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

sds edc_sandbox_verify_path_at(struct edc_config *conf, const char *base, const char *path, bool writable,
                               EdcError **error)
{
    if (!path || !path[0] || !base || !base[0] || path[0] == '/')
        return edc_sandbox_verify_path(conf, path, writable, error);

    sds joined = edc_path_join(base, path, NULL);
    sds result = edc_sandbox_verify_path(conf, joined, writable, error);
    sdsfree(joined);
    return result;
}

/*
 * Apply every kernel sandbox layer to the *current process*: mount-namespace
 * (with deny masking), Landlock, then seccomp-BPF.
 *
 * ⚠️ Reserved, currently UNUSED. The layers are irreversible and inherited, so
 * this must only ever run in the confined process -- never in the agent (it
 * would sandbox the agent itself), and never between fork() and exec() in the
 * multithreaded agent (only async-signal-safe calls are allowed there, and
 * these backends log and allocate). The active path applies the same sequence
 * in the separate, single-threaded sandbox helper right before it execs /bin/sh
 * (see src/agent/sandbox_helper.c); that code cannot call this function because
 * the helper links only the tiny //src/agent:sandbox_backends library, not
 * //src/agent:agent. Keep the two sequences in sync: namespace first (it needs
 * unshare()/mount(), which the seccomp filter denies), then Landlock, then
 * seccomp with block_metadata = !ns_enforced.
 *
 * Returns true when the sandbox is disabled or at least one kernel layer took
 * effect; false only when nothing could be applied and enforcement falls back
 * to path validation alone.
 */
bool edc_sandbox_apply(struct edc_config *conf)
{
#ifdef __linux__
    if (!conf->enable_sandbox || !conf->sandbox_root_real)
    {
        log_info("edc_sandbox: sandbox not enabled, not applied");
        return true;
    }

    /* seccomp filter installation requires CAP_SYS_ADMIN or NO_NEW_PRIVS; set
     * it here so the seccomp layer works even when Landlock is unavailable.
     * (The sandbox helper does the same.) */
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    /*
     * Layer order matters: the mount-namespace backend needs unshare()/mount(),
     * which the seccomp filter denies, so it runs FIRST. When it is enforced the
     * read-only paths are immutable at the VFS level (EROFS) and the path-blind
     * seccomp metadata rules are unnecessary; otherwise the seccomp filter
     * blocks those mutators globally as a fallback.
     */
    bool ns_enforced = false;
    bool ns_ok = edc_ns_sandbox_apply((const char **)conf->sandbox_ro_paths, (const char **)conf->sandbox_rw_paths,
                                      (const char **)conf->sandbox_deny_paths, &ns_enforced);
    bool landlock_ok =
        edc_landlock_sandbox_apply((const char **)conf->sandbox_ro_paths, (const char **)conf->sandbox_rw_paths);
    bool seccomp_ok = edc_bpf_sandbox_apply_ex(conf->sandbox_root_real, !ns_enforced);

    log_info("edc_sandbox: kernel layers ns=%s(%s) landlock=%s seccomp=%s", ns_ok ? "on" : "off",
             ns_enforced ? "enforced" : "not-enforced", landlock_ok ? "on" : "off", seccomp_ok ? "on" : "off");

    if (!ns_enforced && !landlock_ok && !seccomp_ok)
    {
        log_warn("edc_sandbox: no kernel sandbox available; relying on path validation only");
        return false;
    }

    return true;
#else
    (void)conf;
    return true;
#endif
}
