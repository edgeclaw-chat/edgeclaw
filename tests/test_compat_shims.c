/* The behaviour of the platform shims, pinned where they had actually diverged.
 *
 * src/common/win_compat and src/common/edc_compat.h exist so that POSIX-flavoured code
 * compiles on MSVC. The danger in a shim is not that it is missing -- that fails loudly at
 * compile or link time -- but that it is SUBTLY DIFFERENT from the function it stands in for,
 * because then the code that uses it is silently wrong on one platform only. Four such
 * differences were found on Windows and each one is pinned below, in the terms the caller
 * relies on rather than in terms of the implementation:
 *
 *   1. unsetenv() must reach the same environment getenv() reads. It used to be
 *      SetEnvironmentVariable(name, NULL), which updates the Win32 block only, so a variable
 *      looked unset to the OS and still set to the program.
 *   2. realpath() must FAIL for a path that does not exist. It used to be _fullpath, which
 *      only normalises the string and happily returns an absolute path for anything --
 *      and this tree asks "does it exist?" by testing realpath() for NULL in several places.
 *   3. mkdir() must accept the POSIX two-argument form. MSVC's takes one, so a translation
 *      unit that did not include edc_compat.h got "error C2197: too many arguments".
 *   4. lstat()/S_ISLNK() must be able to see a link. They were stubbed as stat() (which
 *      FOLLOWS the link) and a constant 0 (never a link), which silently disarmed
 *      edc_state_store.c's refuse_symlink() -- the check that stops a model-writable
 *      workspace from redirecting the database onto another file.
 *
 * Everything here is a statement about the CONTRACT, so the same assertions run on every
 * platform; a shim that drifted on either side fails. Where a platform genuinely cannot
 * provide the facility the case is skipped with a message rather than pretended.
 */
#include "edc_compat.h"
#include "edc_path.h"
#include "edc_file_utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <unistd.h>

#include "test_fs_util.h"

static int passed = 0;
static int failed = 0;
static int skipped = 0;

#define TEST(name) printf("  TEST: %s\n", name)
#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            printf("    FAIL: ");                                                                                      \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)
#define SKIP(...)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("    skip: ");                                                                                          \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        skipped++;                                                                                                     \
    } while (0)

static char g_dir[1024];

/* ── 1. unsetenv() must reach getenv() ────────────────────────────────────────────────────
 * The regression was environmental, not file-system: setenv() then unsetenv() left the
 * variable visible to the program on Windows. A test that only checks setenv() cannot see it,
 * so the round trip is what matters. */
static void test_env_round_trip(void)
{
    TEST("setenv -> getenv -> unsetenv -> getenv sees nothing (unsetenv reaches the CRT)");

    CHECK(setenv("EDC_COMPAT_PROBE", "v1", 1) == 0, "setenv succeeds");
    const char *v = getenv("EDC_COMPAT_PROBE");
    CHECK(v && strcmp(v, "v1") == 0, "getenv reads what setenv wrote, got %s", v ? v : "(null)");

    /* overwrite=0 must not clobber an existing value; overwrite=1 must. */
    CHECK(setenv("EDC_COMPAT_PROBE", "v2", 0) == 0, "setenv without overwrite is not an error");
    v = getenv("EDC_COMPAT_PROBE");
    CHECK(v && strcmp(v, "v1") == 0, "overwrite=0 keeps the old value, got %s", v ? v : "(null)");
    CHECK(setenv("EDC_COMPAT_PROBE", "v2", 1) == 0, "setenv with overwrite succeeds");
    v = getenv("EDC_COMPAT_PROBE");
    CHECK(v && strcmp(v, "v2") == 0, "overwrite=1 replaces it, got %s", v ? v : "(null)");

    /* THE PIN: before this was fixed the value stayed readable here. */
    CHECK(unsetenv("EDC_COMPAT_PROBE") == 0, "unsetenv succeeds");
    v = getenv("EDC_COMPAT_PROBE");
    CHECK(v == NULL, "after unsetenv getenv must answer NULL, got %s", v ? v : "(null)");

    /* Unsetting something that was never set is not an error (POSIX says so). */
    CHECK(unsetenv("EDC_COMPAT_PROBE_NEVER_SET") == 0, "unsetenv of a missing name is not an error");
}

/* ── 2. realpath() must fail for a path that does not exist ───────────────────────────────
 * This tree asks "does it exist?" by testing realpath() for NULL: edc_file_index_query()
 * returns no rows for an unresolvable directory part, edc_path_guard_resolve() refuses a
 * path whose parent cannot be resolved, and edc_config validates its lists that way. A
 * realpath() that always succeeds makes all of them answer "yes" on Windows. */
static void test_realpath_reports_existence(void)
{
    TEST("realpath resolves what exists and returns NULL for what does not");

    char existing[1024];
    snprintf(existing, sizeof(existing), "%s/real", g_dir);
    test_mkdir_p(existing);

    {   /* the caller-allocated form */
        char buf[4096];
        char *r = realpath(existing, buf);
        CHECK(r != NULL, "an existing directory resolves");
        CHECK(r && edc_path_is_absolute(r), "and the result is absolute: %s", r ? r : "(null)");
    }
    {   /* the malloc form -- what most callers here use */
        char *r = realpath(existing, NULL);
        CHECK(r != NULL, "the NULL-buffer form also resolves");
        free(r);
    }

    char missing[1024];
    snprintf(missing, sizeof(missing), "%s/does-not-exist-%ld", g_dir, (long)getpid());
    {
        char buf[4096];
        char *r = realpath(missing, buf);
        CHECK(r == NULL, "THE PIN: a missing path must not resolve, got %s", r ? r : "(null)");
    }
    {
        char *r = realpath(missing, NULL);
        CHECK(r == NULL, "and the NULL-buffer form fails too, got %s", r ? r : "(null)");
        free(r);
    }

    /* A file that exists is also resolvable, and its parent directory too. */
    char file[1024];
    snprintf(file, sizeof(file), "%s/real/f.txt", g_dir);
    FILE *f = fopen(file, "w");
    if (f)
    {
        fputs("x", f);
        fclose(f);
        char *r = realpath(file, NULL);
        CHECK(r != NULL, "an existing file resolves");
        free(r);
    }
}

/* ── 3. mkdir() takes a mode, as every caller writes it ───────────────────────────────────
 * This is a compile-time pin as much as a runtime one: on Windows the two-argument form only
 * exists through the shim, so a translation unit that includes <unistd.h> but not
 * edc_compat.h stops compiling if the shim loses it (error C2197). */
static void test_mkdir_two_arguments(void)
{
    TEST("mkdir(path, mode) creates a directory and reports EEXIST the second time");

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/mkd", g_dir);
    (void)rmdir(dir);

    CHECK(mkdir(dir, 0755) == 0, "mkdir with a mode succeeds");
    struct stat st;
    CHECK(stat(dir, &st) == 0 && S_ISDIR(st.st_mode), "and it is a directory");

    errno = 0;
    CHECK(mkdir(dir, 0755) != 0, "the second mkdir fails");
    CHECK(errno == EEXIST, "with EEXIST, got errno=%d", errno);
}

/* ── 4. lstat()/S_ISLNK() must see a link ────────────────────────────────────────────────
 * This is the one that was a security hole rather than an inconvenience: with
 * `#define lstat stat` and `#define S_ISLNK(m) 0` the "refuse a symlinked database" check
 * could never fire on Windows. The assertions are ordered so that the cheap cases run
 * everywhere and only the link case is skipped when the platform will not make one. */
static void test_lstat_sees_links(void)
{
    TEST("lstat distinguishes a regular file, a directory and a link");

    char regular[1024], dir[1024];
    snprintf(regular, sizeof(regular), "%s/lst-regular.txt", g_dir);
    snprintf(dir, sizeof(dir), "%s/lst-dir", g_dir);
    test_mkdir_p(dir);

    FILE *f = fopen(regular, "w");
    CHECK(f != NULL, "the fixture file was written");
    if (f)
    {
        fputs("x", f);
        fclose(f);
    }

    struct stat st;
    CHECK(lstat(regular, &st) == 0, "lstat works on a regular file");
    CHECK(S_ISREG(st.st_mode), "which reports a regular file");
    CHECK(!S_ISLNK(st.st_mode), "and not a link");

    CHECK(lstat(dir, &st) == 0, "lstat works on a directory");
    CHECK(S_ISDIR(st.st_mode), "which reports a directory");
    CHECK(!S_ISLNK(st.st_mode), "and not a link");

    /* A path that is not there must fail, so a caller can tell "absent" from "present". */
    char missing[1024];
    snprintf(missing, sizeof(missing), "%s/lst-missing", g_dir);
    CHECK(lstat(missing, &st) != 0, "lstat fails for a missing path");

    /* ── the actual link ──────────────────────────────────────────────────────────────
     * CreateSymbolicLinkW needs SeCreateSymbolicLinkPrivilege (administrator or Developer
     * Mode). A directory JUNCTION does not, and a junction is a reparse point just like a
     * symbolic link -- so on a CI runner without the privilege the junction form still
     * exercises the code path this test exists for, instead of skipping it. */
    char link[1024];
    snprintf(link, sizeof(link), "%s/lst-link", g_dir);
    (void)rmdir(link);
    (void)remove(link);

    int made = (symlink(dir, link) == 0);
    if (!made)
    {
        /* Fall back to a junction: mklink /J needs no privilege on NTFS. */
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "mklink /J \"%s\" \"%s\" >nul 2>&1", link, dir);
        made = (system(cmd) == 0);
    }

    if (!made)
    {
        SKIP("this machine will not create a symlink or a junction (needs a privilege, or "
             "the filesystem does not support reparse points) -- S_ISLNK is unverified here");
        return;
    }

    CHECK(lstat(link, &st) == 0, "lstat works on a link");
    CHECK(S_ISLNK(st.st_mode),
          "THE PIN: a link must be reported as a link, not as its target (S_ISLNK saw %d)",
          (int)S_ISLNK(st.st_mode));
    /* The other half of lstat: stat() FOLLOWS it, so the two must disagree here. Without that
     * the "link" might have been created as something else and this test would prove nothing. */
    struct stat followed;
    CHECK(stat(link, &followed) == 0, "stat follows the link");
    CHECK(!S_ISLNK(followed.st_mode), "and reports the target, not a link");
}

/* ── 5. mkdtemp builds on what the platform actually has ──────────────────────────────────
 * Not a shim difference but the assumption behind it: every scratch directory in this suite
 * comes from a template, and a template whose parent does not exist fails. On Windows "/tmp"
 * means "\tmp" on the CURRENT DRIVE, which a fresh machine does not have -- so the helper
 * must build under TEST_TMPDIR (or the platform's temp root), never a POSIX literal. */
static void test_mkdtemp_under_the_scratch_root(void)
{
    TEST("mkdtemp creates a directory whose parent the platform really has");

    char tmpl[1024];
    test_tmp_template(tmpl, sizeof(tmpl), "compat_mkdt");
    CHECK(edc_path_is_absolute(tmpl), "the template is absolute: %s", tmpl);
    CHECK(strstr(tmpl, "/tmp/") == NULL || strncmp(tmpl, "\\tmp", 4) != 0,
          "the template does not assume /tmp: %s", tmpl);

    char *made = mkdtemp(tmpl);
    CHECK(made != NULL, "mkdtemp succeeds (%s)", tmpl);
    if (made)
    {
        struct stat st;
        CHECK(stat(made, &st) == 0 && S_ISDIR(st.st_mode), "and the directory exists");
    }

    /* A second call must produce a DIFFERENT directory, or two tests would share a tree. */
    char tmpl2[1024];
    test_tmp_template(tmpl2, sizeof(tmpl2), "compat_mkdt2");
    char *made2 = mkdtemp(tmpl2);
    CHECK(made2 != NULL, "a second mkdtemp succeeds");
    CHECK(made && made2 && strcmp(made, made2) != 0, "and it is a distinct directory");
}

int main(void)
{
    printf("platform shim contracts\n");

    test_tmp_template(g_dir, sizeof(g_dir), "compat_shims");
    if (!mkdtemp(g_dir))
    {
        perror("mkdtemp");
        return 1;
    }

    test_env_round_trip();
    test_realpath_reports_existence();
    test_mkdir_two_arguments();
    test_lstat_sees_links();
    test_mkdtemp_under_the_scratch_root();

    test_rm_rf(g_dir);

    printf("\n  %d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed > 0 ? 1 : 0;
}
