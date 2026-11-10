#include "edc_sandbox.h"
#include "edc_path_guard.h"
#include "edc_compat.h"
#include "edc_platform.h"
#include "edc_path.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "log.h"
#include <errno.h>
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

    /* No backend on this platform (Windows): nothing below can enforce anything, and the
     * switch is already clamped off by edc_config_load / refused by /sandbox. Reaching here
     * with it on means the flag was set by hand -- say so once instead of letting the app
     * believe it is sandboxed. Returns success: there is no work to do, not a failure. */
    if (!edc_platform_has_sandbox())
    {
        if (conf->enable_sandbox)
            log_warn("edc_sandbox: no sandbox backend on this platform -- the sandbox is NOT enforced");
        return true;
    }

#if defined(__linux__) || defined(__APPLE__)
    if (conf->enable_sandbox)
    {
        /* Kernel isolation is applied per-command inside the embedded sandbox
         * helper (see src/sandbox/sandbox_helper.c), NOT in the app process:
         * the rules are irreversible and inherited (Landlock/seccomp on Linux,
         * a Seatbelt profile on macOS), so applying them here would sandbox the
         * app itself. In-process enforcement is the tool-layer path
         * verification in edc_sandbox_verify_path_at(). */
        log_info("edc_sandbox: kernel isolation runs in the per-command helper; "
                 "path validation is the in-process enforcement");
    }
#endif

    if (conf->enable_sandbox)
        edc_sandbox_report_unresolved_paths(conf);

    log_info("edc_sandbox: initialized, root = %s", conf->sandbox_root_real);
    return true;
}

size_t edc_sandbox_report_unresolved_paths(struct edc_config *conf)
{
    if (!conf || !conf->enable_sandbox || !conf->sandbox_deny_paths)
        return 0;

    size_t bad = 0;
    for (int i = 0; conf->sandbox_deny_paths[i]; i++)
    {
        char *real = realpath(conf->sandbox_deny_paths[i], NULL);
        if (real)
        {
            free(real);
            continue;
        }
        bad++;
        log_warn("sandbox: deny entry '%s' does not resolve (%s) -- it matches nothing, so what it "
                 "names is NOT masked",
                 conf->sandbox_deny_paths[i], strerror(errno));
    }
    return bad;
}

const char *edc_sandbox_get_root(struct edc_config *conf)
{
    return conf->sandbox_root_real;
}

/* Length of the longest real path prefix in `paths` that contains `resolved_path`, or -1
 * when no entry matches. The LENGTH is what path_allowed() compares, so the most specific
 * entry wins; the whole-segment / separator / case rules come from edc_path_prefix_len.
 * This used to be a local strncmp that knew only '/', so on Windows no entry below the
 * sandbox root ever matched and every path under a writable root was refused. */
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

        const int len = edc_path_prefix_len(resolved_path, entry_real);
        if (len > best)
            best = len;
        free(entry_real);
    }

    return best;
}

/* The LIST half of the policy. The deny check that used to sit here has moved into
 * edc_path_guard_resolve() (rules.apply_deny), so there is one implementation of "deny
 * wins" and the mechanism (resolution, symlinks, containment) lives in one place too.
 * Deny is still consulted first by the guard, before the lists below. */
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

static sds sandbox_verify(struct edc_config *conf, const char *base, const char *path, bool writable,
                          EdcError **error)
{
    if (error)
        *error = NULL;

    /* Sandbox off = no enforcement at all, and the path comes back as given (a relative
     * one still anchored at |base|, which is what "the session's directory" means to
     * every caller) -- the tools then fail on their own terms (a missing file reports
     * "cannot read"), and callers that care about the string are unaffected.
     *
     * "As given" means an ABSOLUTE path stays absolute, and edc_path_is_absolute is what
     * knows about drive letters and UNC roots. This used to test path[0] != '/', which
     * read "D:\ws\x" as relative and anchored it at the session directory, yielding
     * "D:\ws\D:\ws\x" -- the same failure mode that broke `session new D:\ws`. The
     * branch is the ONLY one Windows runs (no backend -> the switch is always off, see
     * edc_platform_has_sandbox), so every absolute path handed to read_file / write_file /
     * list_dir / edit_file / an @file mention was mangled there. */
    if (!conf->enable_sandbox || !conf->sandbox_root_real)
    {
        if (base && base[0] && path && path[0] && !edc_path_is_absolute(path))
            return edc_path_join(base, path, NULL);
        return sdsnew(path ? path : "");
    }

    if (!path || !path[0])
    {
        edc_error_set(error, EDC_ERR_SANDBOX, "empty path");
        return NULL;
    }

    /* Mechanism first (resolve + symlink rule + deny), then the lists. Writes never go
     * through a symlink; reads may follow one (the sandbox checks the target). */
    struct edc_path_rules rules = {
        .base = base,
        .root = NULL, /* containment is expressed by the rw/ro lists below, not a root */
        .writable = writable,
        .refuse_symlink = writable,
        /* Reads refuse a hardlink, writes do not. The deny list matches PATHS, so
         * `ln ~/.ssh/id_ed25519 ~/ws/innocent` used to hand the model the key under a name
         * no deny entry covers -- the file half of what this mask is for. Writes are left
         * alone on purpose: refusing nlink > 1 there would break legitimate trees that
         * hardlink their outputs, and the write path already has the shrink-retry ladder.
         * Directories are exempt inside the guard (a dir's nlink counts its children). */
        .refuse_hardlink = !writable,
        .deny_paths = conf->sandbox_deny_paths,
    };
    sds resolved = edc_path_guard_resolve(&rules, path, error);
    if (!resolved)
        return NULL;

    if (!path_allowed(conf, resolved, writable))
    {
        edc_error_set(error, EDC_ERR_SANDBOX, "access denied: '%s' is outside sandbox", path);
        sdsfree(resolved);
        return NULL;
    }
    return resolved;
}

sds edc_sandbox_verify_path_at(struct edc_config *conf, const char *base, const char *path, bool writable,
                               EdcError **error)
{
    /* A relative path is resolved against |base| (the session's directory); an absolute one
     * ignores it. The rules live in sandbox_verify/edc_path_guard. */
    return sandbox_verify(conf, (base && base[0]) ? base : NULL, path, writable, error);
}

