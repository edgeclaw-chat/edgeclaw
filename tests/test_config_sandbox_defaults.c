/* Platform defaults for the sandbox path lists, and the migration of a
 * config.json written on the other platform.
 *
 * The ro whitelist is deliberately not the same everywhere (see
 * EDC_SANDBOX_DEFAULT_RO_PATHS in edc_config.h): /proc, /sys, /run and /lib do
 * not exist on macOS, where a shell command instead needs /System (the dyld
 * shared cache), /Library (Frameworks) and /Applications (the git/clang/make
 * shims dlopen the Xcode toolchain from there). The writable set additionally
 * needs the per-boot $TMPDIR on macOS, because xcrun and the compilers write
 * their caches under it and edc_shell_env forwards TMPDIR to every command.
 *
 * The migration test is the interesting half: a config.json carried over from
 * Linux keeps /proc, which can never resolve on macOS, so every read that entry
 * was meant to grant would silently be missing. The loader keys off exactly that
 * and tops the whitelist back up. It must NOT fire on Linux.
 */

#include "edc_config.h"
#include "edc_init_env.h"
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0;
static int failed = 0;

#define T_CHECK(cond, msg)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            passed++;                                                                                                  \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* Compare resolved paths, not raw strings.
 *
 * The writable list mixes entries written by edc_init_env -- which emits
 * "<workspace>/" WITH a trailing slash -- with entries built in edc_config, and
 * on macOS realpath() additionally rewrites /tmp to /private/tmp. Both spellings
 * name the same directory, which is what these assertions are about; comparing
 * bytes made the macOS run pass for the wrong reason (the mismatching realpath
 * made the loader append a second, slash-less copy) and the Linux run fail. */
static bool same_path(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    if (strcmp(a, b) == 0)
        return true;

    char ra[PATH_MAX];
    char rb[PATH_MAX];
    if (!realpath(a, ra) || !realpath(b, rb))
        return false;
    return strcmp(ra, rb) == 0;
}

static bool has_path(sds *arr, const char *want)
{
    for (int i = 0; arr && arr[i]; i++)
        if (same_path(arr[i], want))
            return true;
    return false;
}

/* Printed only on failure: a missing entry is far easier to read as the whole
 * list than as one assertion per line. */
static void dump_list(const char *label, sds *arr)
{
    fprintf(stderr, "  %s = [", label);
    for (int i = 0; arr && arr[i]; i++)
        fprintf(stderr, "%s%s", i ? ", " : "", arr[i]);
    fprintf(stderr, "]\n");
}

int main(void)
{
    /* The secret module (api_key encryption) refuses to run without a key when
     * there is no terminal -- the same variable the other config tests set. */
    setenv("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef", 1);

    char home[] = "/tmp/edc_sb_defaults_XXXXXX";
    if (!mkdtemp(home))
    {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    T_CHECK(edc_init_env(home, true), "edc_init_env creates config.json");

    struct edc_config *cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "edc_config_load reads the fresh config back");
    if (!cfg)
        return 1;

    /* ── 1. the shared part of the whitelist ─────────────────────────────── */
    if (!has_path(cfg->sandbox_ro_paths, "/usr") || !has_path(cfg->sandbox_ro_paths, "/bin"))
        dump_list("ro_paths", cfg->sandbox_ro_paths);
    T_CHECK(has_path(cfg->sandbox_ro_paths, "/usr"), "ro has /usr");
    T_CHECK(has_path(cfg->sandbox_ro_paths, "/bin"), "ro has /bin");
    T_CHECK(has_path(cfg->sandbox_rw_paths, "/tmp"), "rw has /tmp");
    T_CHECK(has_path(cfg->sandbox_rw_paths, "/dev"), "rw has /dev");
    T_CHECK(cfg->workspace && has_path(cfg->sandbox_rw_paths, cfg->workspace), "rw has the workspace");

    /* ── 2. the platform-specific part ───────────────────────────────────── */
#if defined(__APPLE__)
    if (!has_path(cfg->sandbox_ro_paths, "/System"))
        dump_list("ro_paths", cfg->sandbox_ro_paths);
    T_CHECK(has_path(cfg->sandbox_ro_paths, "/System"), "macOS ro has /System (dyld shared cache)");
    T_CHECK(has_path(cfg->sandbox_ro_paths, "/Library"), "macOS ro has /Library (Frameworks)");
    T_CHECK(has_path(cfg->sandbox_ro_paths, "/Applications"), "macOS ro has /Applications (Xcode shims)");
    /* None of these can resolve here, so keeping them would grant nothing and
     * only hide which root is actually missing. */
    T_CHECK(!has_path(cfg->sandbox_ro_paths, "/proc"), "macOS ro has no /proc");
    T_CHECK(!has_path(cfg->sandbox_ro_paths, "/sys"), "macOS ro has no /sys");
    T_CHECK(!has_path(cfg->sandbox_ro_paths, "/lib"), "macOS ro has no /lib");

    const char *tmpdir = edc_sandbox_default_tmpdir();
    if (!tmpdir || !has_path(cfg->sandbox_rw_paths, tmpdir))
        dump_list("rw_paths", cfg->sandbox_rw_paths);
    T_CHECK(tmpdir != NULL, "macOS $TMPDIR is set");
    T_CHECK(tmpdir && has_path(cfg->sandbox_rw_paths, tmpdir), "macOS rw has $TMPDIR");
#else
    T_CHECK(has_path(cfg->sandbox_ro_paths, "/proc"), "Linux ro has /proc");
    T_CHECK(!has_path(cfg->sandbox_ro_paths, "/System"), "Linux ro has no /System");
    T_CHECK(edc_sandbox_default_tmpdir() == NULL, "TMPDIR is a macOS-only rw entry");
#endif

    /* ── 3. migration of a config.json written on the other platform ─────── */
    /* Simulate it: no /System (it is not in the Linux whitelist), plus the
     * /proc marker. */
    T_CHECK(edc_config_update_sandbox_path(home, "/System", EDC_SANDBOX_PATH_RO, false),
            "drop /System from the persisted ro list");
    T_CHECK(edc_config_update_sandbox_path(home, "/proc", EDC_SANDBOX_PATH_RO, true),
            "inject the Linux-only /proc marker");

    edc_config_free(cfg);
    cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "edc_config_load reads the edited config back");
    if (!cfg)
        return 1;

#if defined(__APPLE__)
    if (!has_path(cfg->sandbox_ro_paths, "/System"))
        dump_list("ro_paths", cfg->sandbox_ro_paths);
    T_CHECK(has_path(cfg->sandbox_ro_paths, "/System"),
            "macOS migration restores /System when /proc is present");
    /* The marker itself is harmless but meaningless here; the migration tops up,
     * it does not scrub, because only the user knows whether an unresolvable
     * entry was a mistake or a mount that appears later. */
    T_CHECK(has_path(cfg->sandbox_ro_paths, "/Library"), "macOS migration restores /Library");
#else
    T_CHECK(!has_path(cfg->sandbox_ro_paths, "/System"),
            "the migration is macOS-only: Linux keeps its list untouched");
#endif

    edc_config_free(cfg);

    /* Best-effort cleanup (the other config tests leave their home behind too). */
    char conf_path[sizeof(home) + 16];
    snprintf(conf_path, sizeof(conf_path), "%s/config.json", home);
    remove(conf_path);
    rmdir(home);

    if (failed)
    {
        fprintf(stderr, "%d passed, %d FAILED\n", passed, failed);
        return 1;
    }
    printf("%d passed\n", passed);
    return 0;
}
