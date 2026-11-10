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
#include "test_fs_util.h"

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

/* One spelling per directory: no trailing slash (except "/"), no duplicates.
 * The loader canonicalises all three lists at the end of the read, and everything
 * downstream -- the sandbox backends, /sandbox list, the strcmp-based dedup in
 * sds_array_add_unique -- assumes that. */
static bool list_is_canonical(sds *arr)
{
    for (int i = 0; arr && arr[i]; i++)
    {
        size_t len = strlen(arr[i]);
        if (len > 1 && arr[i][len - 1] == '/')
            return false;
        for (int j = 0; j < i; j++)
            if (strcmp(arr[i], arr[j]) == 0)
                return false;
    }
    return true;
}

/* How many entries name the same directory. Used to catch the duplicate that the
 * loader used to append: two spellings of one directory ("<ws>/" from
 * edc_init_env, "<ws>" from the loader) both landed in rw_paths. */
static int count_path(sds *arr, const char *want)
{
    int n = 0;
    for (int i = 0; arr && arr[i]; i++)
        if (same_path(arr[i], want))
            n++;
    return n;
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

    char home[1024];


    test_tmp_template(home, sizeof(home), "edc_sb_defaults");
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
    if (!cfg->workspace || count_path(cfg->sandbox_rw_paths, cfg->workspace) != 1)
    {
        dump_list("rw_paths", cfg->sandbox_rw_paths);
        fprintf(stderr, "  workspace = [%s]\n", cfg->workspace ? cfg->workspace : "(null)");
    }
    T_CHECK(cfg->workspace && has_path(cfg->sandbox_rw_paths, cfg->workspace), "rw has the workspace");
    T_CHECK(count_path(cfg->sandbox_rw_paths, cfg->workspace) == 1,
            "the workspace appears exactly once in rw (no duplicate spelling)");
    if (!list_is_canonical(cfg->sandbox_rw_paths))
        dump_list("rw_paths", cfg->sandbox_rw_paths);
    T_CHECK(list_is_canonical(cfg->sandbox_ro_paths), "ro is canonical: no trailing slash, no duplicates");
    T_CHECK(list_is_canonical(cfg->sandbox_rw_paths), "rw is canonical: no trailing slash, no duplicates");

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

    /* ── 4. canonicalisation collapses two spellings of one directory ─────── */
    /* Persist a trailing-slash spelling next to the canonical one: the loader
     * must hand back a single entry, because that is what every strcmp-based
     * consumer downstream (dedup included) expects. */
    cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "edc_config_load reads the config back");
    if (!cfg)
        return 1;

    char ws_slash[PATH_MAX];
    snprintf(ws_slash, sizeof(ws_slash), "%s/", cfg->workspace);
    T_CHECK(edc_config_update_sandbox_path(home, ws_slash, EDC_SANDBOX_PATH_RW, true),
            "persist a trailing-slash spelling of the workspace");
    edc_config_free(cfg);

    cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "edc_config_load reads the config back again");
    if (!cfg)
        return 1;
    if (count_path(cfg->sandbox_rw_paths, cfg->workspace) != 1)
        dump_list("rw_paths", cfg->sandbox_rw_paths);
    T_CHECK(count_path(cfg->sandbox_rw_paths, cfg->workspace) == 1,
            "two spellings of the workspace collapse into one entry");
    T_CHECK(list_is_canonical(cfg->sandbox_rw_paths), "rw is canonical after the collapse");

    edc_config_free(cfg);

    /* ── 4b. the deny list starts with the sensitive $HOME paths ──────────── */
    /* edc_homedir() follows $HOME, so a scratch home decides what the defaults can see.
     * Only a path that resolves is expected back: an entry naming something this install
     * does not have matches nothing and earns a startup warning, so it is dropped instead
     * of padding the list. */
    char sb_home[1024];
    test_tmp_template(sb_home, sizeof(sb_home), "edc_sb_deny_home");
    if (mkdtemp(sb_home))
    {
        char planted_ssh[PATH_MAX], planted_hist[PATH_MAX], absent[PATH_MAX];
        snprintf(planted_ssh, sizeof(planted_ssh), "%s/.ssh", sb_home);
        snprintf(planted_hist, sizeof(planted_hist), "%s/.zsh_history", sb_home);
        snprintf(absent, sizeof(absent), "%s/.gnupg", sb_home);
        test_mkdir(planted_ssh);
        FILE *fh = fopen(planted_hist, "w");
        if (fh)
        {
            fputs("export SOME_TOKEN=not-a-real-one\n", fh);
            fclose(fh);
        }

        char saved_home[PATH_MAX];
        const char *old_home = getenv("HOME");
        snprintf(saved_home, sizeof(saved_home), "%s", old_home ? old_home : "");
        setenv("HOME", sb_home, 1);

        sds *defs = edc_config_default_deny_paths();
        if (!has_path(defs, planted_ssh) || !has_path(defs, planted_hist))
        {
            dump_list("default deny", defs);
            fprintf(stderr, "  $HOME = [%s]\n", sb_home);
        }
        T_CHECK(has_path(defs, planted_ssh), "the defaults deny ~/.ssh");
        T_CHECK(has_path(defs, planted_hist), "the defaults deny ~/.zsh_history");
        T_CHECK(!has_path(defs, absent), "a default naming a path that does not exist is dropped");
        T_CHECK(list_is_canonical(defs), "the default deny list is canonical");
        for (int i = 0; defs && defs[i]; i++)
            sdsfree(defs[i]);
        free(defs);

        /* ... and the seed reaches a freshly written config.json. */
        char seeded[1024];
        test_tmp_template(seeded, sizeof(seeded), "edc_sb_deny_seed");
        if (mkdtemp(seeded))
        {
            T_CHECK(edc_init_env(seeded, true), "edc_init_env writes a config.json");
            struct edc_config *sc = edc_config_load(seeded);
            T_CHECK(sc != NULL, "the seeded config reads back");
            if (sc)
            {
                if (!has_path(sc->sandbox_deny_paths, planted_ssh))
                    dump_list("seeded deny_paths", sc->sandbox_deny_paths);
                T_CHECK(has_path(sc->sandbox_deny_paths, planted_ssh), "config.json deny_paths has ~/.ssh");
                T_CHECK(has_path(sc->sandbox_deny_paths, planted_hist), "config.json deny_paths has ~/.zsh_history");
                T_CHECK(list_is_canonical(sc->sandbox_deny_paths), "the seeded deny list is canonical");
                edc_config_free(sc);
            }
            test_rm_rf(seeded);
        }

        setenv("HOME", saved_home, 1);
        test_rm_rf(sb_home);
    }

    /* ── 4c. a deny entry may name a single FILE ──────────────────────────── */
    /* That is the case deny exists for: the secrets worth masking are files in
     * directories the model is meant to keep reading. ro/rw stay directory-only. */
    cfg = edc_config_load(home);
    if (cfg)
    {
        char denied_file[PATH_MAX];
        snprintf(denied_file, sizeof(denied_file), "%s/its_a_secret", home);
        FILE *sf = fopen(denied_file, "w");
        if (sf)
        {
            fputs("not a real secret\n", sf);
            fclose(sf);
        }

        sds msg = NULL;
        bool ok = edc_config_set_sandbox_path(cfg, denied_file, EDC_SANDBOX_PATH_DENY, true, &msg);
        if (!ok)
            fprintf(stderr, "  deny of a file failed: %s\n", msg ? msg : "(no message)");
        T_CHECK(ok, "a regular file can be added to the deny list");
        T_CHECK(has_path(cfg->sandbox_deny_paths, denied_file), "the denied file is in deny_paths");
        if (msg)
            sdsfree(msg);
        msg = NULL;

        ok = edc_config_set_sandbox_path(cfg, denied_file, EDC_SANDBOX_PATH_RW, true, &msg);
        T_CHECK(!ok, "rw still refuses a regular file");
        if (msg)
            sdsfree(msg);
        msg = NULL;

        ok = edc_config_set_sandbox_path(cfg, "/no/such/path/anywhere", EDC_SANDBOX_PATH_DENY, true, &msg);
        T_CHECK(!ok, "denying a path that does not exist is reported, not accepted silently");
        if (msg)
            sdsfree(msg);

        edc_config_free(cfg);
    }

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
