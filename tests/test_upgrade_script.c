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
 * The .ps1 extension is not cosmetic: `powershell -File <path>` refuses any other extension
 * ("the file does not have a '.ps1' extension"), so a random name without it would fail one
 * step later than the bug being fixed here.
 */

#include "edc_upgrade.h"
#include "sds.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
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

/* Read the whole file. Returns the number of bytes read, or -1. */
static long slurp(const char *path, char *buf, size_t cap)
{
#if defined(_WIN32)
    FILE *f = fopen(path, "rb");
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f)
        return -1;
    const size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

static bool file_exists(const char *path)
{
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path, F_OK) == 0;
#endif
}

static bool ends_with(const char *s, const char *suffix)
{
    const size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls)
        return false;
    return strcmp(s + ls - lx, suffix) == 0;
}

int main(void)
{
    /* A body with a newline and the CRLF shape a fetched script actually has: a writer that
     * treats the body as a C string would silently stop at the first NUL, and one that skips
     * bytes would be caught by the length check rather than by a diff nobody reads. */
    const char *body = "Write-Host 'edgeclaw'\r\n\r\nexit 0\r\n";

    sds path = edc_upgrade_stage_script(body);
    CHECK(path != NULL, "a script can be staged");

    if (path)
    {
        CHECK(file_exists(path), "the staged file exists");
        CHECK(ends_with(path, ".ps1"),
              "the staged name ends in .ps1, which is what powershell -File accepts");

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

#if !defined(_WIN32)
        struct stat st;
        if (stat(path, &st) == 0)
            CHECK((st.st_mode & 0777) == 0700, "the staged script is private (0700)");
        else
            CHECK(0, "the staged script is private (0700)");
        CHECK(strstr(path, "edgeclaw-install-") != NULL, "the staged name identifies its origin");
#endif

        edc_upgrade_discard_script(path);
        CHECK(!file_exists(path), "discarding removes the file");
        /* Idempotent: a second removal (or a restart that retries teardown) must not abort. */
        edc_upgrade_discard_script(path);
        edc_upgrade_discard_script(NULL);
        CHECK(1, "discarding again, or with NULL, is harmless");
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

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
