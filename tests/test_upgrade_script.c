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
#else
#include <stdlib.h>
#include <unistd.h>
#endif

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

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
