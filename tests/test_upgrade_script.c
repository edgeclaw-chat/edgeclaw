/* Staging the installer into a temporary file.
 *
 * This is the step that made /update fail on every Windows install: the writer used to be
 * private to edc_upgrade_run.c and its `#ifdef _WIN32` branch returned NULL, on the grounds
 * that "there is no shell to run the script anyway". That was true until the PowerShell
 * installer was added, and it stayed behind -- so the fetch succeeded and the upgrade then
 * died with "cannot write the installer to a temporary file", before anything ran.
 *
 * Nothing caught it because tests/test_upgrade_run.c and tests/test_upgrade.c are both
 * POSIX_ONLY, so neither has ever been compiled on Windows. This target deliberately is not:
 * the property under test is per-platform (a name, a mode, a claim), and a test that skips
 * the platform where the bug lives is the one that lets it back in.
 *
 * The implementation is now one body with the four file calls spelled per platform, so this
 * file measures the same logic on both -- the failure paths included, which is why they are
 * exercised here rather than left to the one platform that would notice.
 */

#include "edc_upgrade.h"
#include "sds.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>

static void set_env(const char *name, const char *value)
{
    _putenv_s(name, value ? value : "");
}
#else
#include <stdlib.h>
#include <unistd.h>

static void set_env(const char *name, const char *value)
{
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
}
#endif

/* The value of NAME in a NULL-terminated "NAME=value" array, or NULL.
 *
 * Windows environment names are case-insensitive, and the spelling in the block is whatever
 * created the entry: bazel's test runner sets SYSTEMROOT, so _putenv_s("SystemRoot", ...)
 * updates the value and leaves the name in capitals. Comparing case-sensitively made this
 * test pass when run by hand (no SYSTEMROOT there, so the name was created as written) and
 * fail under `bazel test` -- the opposite of the failure it is supposed to catch. */
static const char *env_lookup(char *const *env, const char *name)
{
    const size_t n = strlen(name);
    for (size_t i = 0; env && env[i]; i++)
#if defined(_WIN32)
        if (_strnicmp(env[i], name, n) == 0 && env[i][n] == '=')
#else
        if (strncmp(env[i], name, n) == 0 && env[i][n] == '=')
#endif
            return env[i] + n + 1;
    return NULL;
}

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            g_passed++;                                                                                                \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            g_failed++;                                                                                                \
        }                                                                                                              \
    } while (0)

static long slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    const size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

/* Portable existence check: a file this code creates is a readable regular file, so opening it
 * answers the question without <windows.h> or <unistd.h> in the way. */
static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

int main(void)
{
    /* A body with a newline and the CRLF shape a fetched script actually has: a writer that
     * skips bytes is caught by the length check rather than by a diff nobody reads. */
    const char *body = "Write-Host 'edgeclaw'\r\n\r\nexit 0\r\n";

    sds path = edc_upgrade_stage_script(body);
    CHECK(path != NULL, "a script can be staged");

    if (path)
    {
        CHECK(file_exists(path), "the staged file exists");
#if defined(_WIN32)
        /* Windows only: `powershell -File` refuses any other extension. The POSIX branch is
         * run as `sh <path>`, where an extension would only be misleading -- it is
         * install.sh, not a .ps1. Asserting this everywhere is what a Linux CI run caught
         * (the first version of this test did exactly that). */
        const size_t lp = strlen(path);
        CHECK(lp > 4 && strcmp(path + lp - 4, ".ps1") == 0,
              "the staged name ends in .ps1, which is what powershell -File accepts");
#endif

        char buf[512];
        const long n = slurp(path, buf, sizeof(buf) - 1);
        CHECK(n == (long)strlen(body), "every byte of the body was written");
        if (n == (long)strlen(body))
        {
            buf[n] = '\0';
            CHECK(memcmp(buf, body, (size_t)n) == 0, "the staged bytes are the fetched bytes");
        }
        else
        {
            CHECK(0, "the staged bytes are the fetched bytes (length mismatch)");
        }

        CHECK(strstr(path, "edgeclaw-install-") != NULL, "the staged name identifies its origin");

#if !defined(_WIN32)
        /* POSIX only: mkstemp used to hand out 0600 and the mode is now set by the create.
         * What matters either way is that nobody else can rewrite the script between the
         * write and the exec. */
        struct stat st;
        if (stat(path, &st) == 0)
            CHECK((st.st_mode & 0777) == 0600, "the staged script is private (0600)");
        else
            CHECK(0, "the staged script is private (0600)");
#endif

        edc_upgrade_discard_script(path);
        CHECK(!file_exists(path), "discarding removes the file");
        /* Idempotent: a second removal (or a restart that retries teardown) must not abort. */
        edc_upgrade_discard_script(path);
        edc_upgrade_discard_script(NULL);
        edc_upgrade_discard_script("");
        CHECK(1, "discarding again, with NULL, or with an empty path, is harmless");
        sdsfree(path);
    }

    /* Two concurrent runs must not be handed the same file: the name is claimed, not
     * guessed. */
    sds a = edc_upgrade_stage_script(body);
    sds b = edc_upgrade_stage_script(body);
    CHECK(a && b, "two scripts can be staged at once");
    if (a && b)
        CHECK(strcmp(a, b) != 0, "each staged script gets its own name");
    edc_upgrade_discard_script(a);
    edc_upgrade_discard_script(b);
    sdsfree(a);
    sdsfree(b);

    /* Refusals, rather than a zero-byte executable in a temp directory. */
    CHECK(edc_upgrade_stage_script(NULL) == NULL, "no body, no file");
    CHECK(edc_upgrade_stage_script("") == NULL, "an empty body is refused");

#if !defined(_WIN32)
    /* The failure paths. They are the reason this belongs in a test rather than in a comment:
     * before, "cannot write the installer to a temporary file" was reported by a branch that
     * was never taken on the platform that could report it.
     *
     * A temp directory that does not exist: the create fails with something other than
     * EEXIST, so the retry loop gives up immediately instead of trying 64 names. */
    const char *real_tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char saved[512];
    snprintf(saved, sizeof(saved), "%s", real_tmp);

    setenv("TMPDIR", "/nonexistent-edgeclaw-test-dir", 1);
    CHECK(edc_upgrade_stage_script(body) == NULL, "an unusable temp directory yields no file");

    /* And the default is used when TMPDIR says nothing, which is the path a stock machine
     * takes. */
    setenv("TMPDIR", "", 1);
    sds d = edc_upgrade_stage_script(body);
    CHECK(d != NULL, "an empty TMPDIR falls back to /tmp");
    if (d)
        CHECK(strncmp(d, "/tmp/", 5) == 0, "the fallback really is /tmp");
    edc_upgrade_discard_script(d);
    sdsfree(d);

    setenv("TMPDIR", saved, 1);

    /* Discarding something that is not a file: unlink fails with EISDIR/EPERM, which is the
     * branch that logs instead of retrying. It must not crash, and must not remove the
     * directory. */
    char dirpath[] = "/tmp/edgeclaw-test-dir-XXXXXX";
    if (mkdtemp(dirpath))
    {
        edc_upgrade_discard_script(dirpath);
        struct stat dst;
        CHECK(stat(dirpath, &dst) == 0 && S_ISDIR(dst.st_mode), "discarding a directory leaves it alone");
        rmdir(dirpath);
    }
    else
    {
        CHECK(0, "could not create a scratch directory for the discard test");
    }
#endif

    /* ── the installer's environment ────────────────────────────────────────
     *
     * What the runner hands the child used to be an allowlist -- proxy variables only. On
     * Windows that meant no PROCESSOR_ARCHITECTURE (install.ps1 refuses to choose a target:
     * "Fail : PROCESSOR_ARCHITECTURE is not set"), no PATHEXT (so `tar` never resolved and
     * the extraction failed), and no TEMP. Three bugs, each found by a user in turn.
     *
     * The rule is now "inherit, minus this program's own variables", so both halves are
     * asserted: what must survive, and what must not. */
    {
        set_env("EDGECLAW_MASTER_KEY", "the-passphrase-or-key");
        set_env("EDGECLAW_SOMETHING_ADDED_LATER", "private-by-default");

        /* The inheritance probes are SET here rather than read from the ambient environment.
         * A bazel test action gets a minimal environment -- measured: this binary sees no
         * PROCESSOR_ARCHITECTURE, PATHEXT or SystemRoot under `bazel test`, and all three when
         * run directly -- so asserting on what happens to be there would make the test pass
         * or fail on the runner rather than on the code. */
        set_env("EDGE_UPGRADE_INHERIT_PROBE", "kept");

        const sds *env = edc_upgrade_child_env();
        char *const *lookup = (char *const *)env;
        CHECK(env != NULL, "an environment is produced at all");

        /* Subtracted: the secret, and anything else added under the prefix later. */
        CHECK(lookup && env_lookup(lookup, "EDGECLAW_MASTER_KEY") == NULL,
              "EDGECLAW_MASTER_KEY does not reach the installer");
        CHECK(lookup && env_lookup(lookup, "EDGECLAW_SOMETHING_ADDED_LATER") == NULL,
              "a variable added later under the prefix is private without anyone listing it");

        /* Inherited: the variables whose absence actually broke the upgrade. */
        CHECK(lookup && env_lookup(lookup, "EDGE_UPGRADE_INHERIT_PROBE") != NULL,
              "an ordinary variable is inherited");

#if defined(_WIN32)
        /* The three whose absence actually broke install.ps1 on Windows, set explicitly so the
         * assertion is about the code and not about the runner's environment. */
        set_env("PROCESSOR_ARCHITECTURE", "AMD64");
        set_env("PATHEXT", ".COM;.EXE;.BAT;.CMD");
        set_env("SystemRoot", "C:\\Windows");
        const sds *win_env = edc_upgrade_child_env();
        char *const *win = (char *const *)win_env;
        CHECK(win && env_lookup(win, "PROCESSOR_ARCHITECTURE") != NULL,
              "PROCESSOR_ARCHITECTURE is inherited (install.ps1 cannot pick a target without it)");
        CHECK(win && env_lookup(win, "PATHEXT") != NULL,
              "PATHEXT is inherited (without it `tar` does not resolve)");
        CHECK(win && env_lookup(win, "SystemRoot") != NULL, "SystemRoot is inherited");
        edc_upgrade_env_free((sds *)win_env);
#endif
        /* The half the old allowlist got right, kept from regressing. */
        set_env("HTTPS_PROXY", "http://127.0.0.1:9/");
        const sds *with_proxy = edc_upgrade_child_env();
        CHECK(with_proxy && env_lookup((char *const *)with_proxy, "HTTPS_PROXY") != NULL,
              "the proxy variables still reach the installer");
        edc_upgrade_env_free((sds *)with_proxy);
        set_env("HTTPS_PROXY", NULL);

        /* An array the caller owns, freed once; a second pass must be harmless too. */
        edc_upgrade_env_free((sds *)env);
        edc_upgrade_env_free(NULL);

        set_env("EDGECLAW_MASTER_KEY", NULL);
        set_env("EDGECLAW_SOMETHING_ADDED_LATER", NULL);
        set_env("EDGE_UPGRADE_INHERIT_PROBE", NULL);
    }

    /* The rule itself, so a platform that cannot spawn still tests it. */
    CHECK(edc_upgrade_env_is_private("EDGECLAW_MASTER_KEY=x"), "the master key is private");
    CHECK(edc_upgrade_env_is_private("EDGECLAW_=x"), "the bare prefix counts as private");
    CHECK(!edc_upgrade_env_is_private("PATH=/usr/bin"), "PATH is not private");
    CHECK(!edc_upgrade_env_is_private("PROCESSOR_ARCHITECTURE=AMD64"),
          "PROCESSOR_ARCHITECTURE is not private");
    CHECK(!edc_upgrade_env_is_private("XEDGECLAW_A=b"),
          "a name that merely contains the prefix is not private");
    CHECK(edc_upgrade_env_is_private(NULL), "NULL is treated as private, not dereferenced");

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
