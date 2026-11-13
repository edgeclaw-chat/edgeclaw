/* test_version: the version banner and the --help block.
 *
 * Why this file exists: `--help` listed three options while the binary answered six. The
 * three it listed were the ones that existed when the text was written; `--update-check`
 * arrived later and was never added, so the one place a user looks for it did not mention
 * it. Nothing failed -- the help was complete as far as any test knew, because no test
 * looked at it.
 *
 * The assertion below is therefore not "the help says these words". It is a CROSS-CHECK
 * against the parser: every flag `main.cpp` compares argv against must appear in the help
 * text. The list of flags is parsed out of main.cpp at test time, so adding an option
 * without documenting it fails here rather than in a user's terminal.
 *
 * That is why this test reads a source file. A hand-maintained list of expected flags
 * would be a third place to forget -- main.cpp, the help text, and the test -- and the
 * whole point is to remove the third.
 */

#include "edc_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_fs_util.h"

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: %s\n", msg);                                                                    \
            g_failed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("    ok: %s\n", msg);                                                                               \
            g_passed++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* Render the help block into a freshly allocated string. */
static char *render_help(void)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;
    edc_usage_print("edgeclaw", f);
    fclose(f);
    return buf;
}

/* Read a whole file; NULL when it cannot be opened (a failure the caller reports). */
static char *read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    if (out_len)
        *out_len = (long)got;
    return buf;
}

/* Does |hay| contain |needle|? strstr, spelled out so the call sites read as assertions. */
static int contains(const char *hay, const char *needle) { return hay && needle && strstr(hay, needle) != NULL; }

/* Longest flag string the help block will ever hold, including its "<value>" placeholder.
 * Generous: this bounds a buffer, it does not describe the format. */
#define FLAG_MAX 48

/* Collect the long options main.cpp compares argv against.
 *
 * Two shapes are used in that file and both must be recognised, or the check silently
 * misses half the flags (which it did on the first attempt: --no-sandbox and --sandbox are
 * matched as `arg == "--..."` on a std::string, not through strcmp):
 *
 *   strcmp(argv[k], "--update-check") == 0
 *   arg == "--no-sandbox"
 *   arg.rfind("--sandbox=", 0) == 0        -> recorded as --sandbox
 *
 * Returns how many were found. */
static int flags_in_parser(const char *src, char flags[][FLAG_MAX], int cap)
{
    static const char *const needles[] = {"strcmp(argv[", "arg == \"", "arg.rfind(\""};
    int n = 0;

    for (size_t pat = 0; pat < sizeof(needles) / sizeof(needles[0]); pat++)
    {
        const char *p = src;
        while ((p = strstr(p, needles[pat])) != NULL)
        {
            p += strlen(needles[pat]);
            /* The quote that opens the flag literal: already consumed for the `arg ==`
             * shapes, still ahead for strcmp. */
            const char *q = (needles[pat][strlen(needles[pat]) - 1] == '"') ? p - 1 : strchr(p, '"');
            if (!q)
                continue;
            const char *end = strchr(q + 1, '"');
            if (!end)
                continue;

            size_t len = (size_t)(end - q - 1);
            if (len > 2 && q[1] == '-' && q[2] == '-')
            {
                /* `--sandbox=` is one option with a value; keep the name so the help is
                 * checked for the flag and not for its exact flag=value spelling. */
                for (size_t i = 0; i < len; i++)
                    if (q[1 + i] == '=')
                    {
                        len = i;
                        break;
                    }

                int seen = 0;
                for (int i = 0; i < n; i++)
                    if (strncmp(flags[i], q + 1, len) == 0 && flags[i][len] == '\0')
                        seen = 1;
                if (!seen && n < cap && len < FLAG_MAX)
                {
                    memcpy(flags[n], q + 1, len);
                    flags[n][len] = '\0';
                    n++;
                }
            }
            p = end;
        }
    }
    return n;
}

int main(void)
{
    printf("\n── the banner names the program and the build ──\n");
    {
        char *help = render_help();
        CHECK(help != NULL, "the help block renders");
        if (help)
        {
            CHECK(contains(help, "edgeclaw"), "it names the program");
            CHECK(contains(help, "Usage: edgeclaw [options]"), "and shows the usage line");
            free(help);
        }
    }

    printf("\n── every option the parser answers is documented ──\n");
    {
        /* Runfiles: the test is executed from the runfiles root, where the main repo is
         * available under its workspace name. Both spellings are tried so this works
         * whether or not Bazel has remapped the layout. */
        const char *candidates[] = {
            "src/tui/main.cpp",
            "_main/src/tui/main.cpp",
            "../edgeclaw/src/tui/main.cpp",
        };
        char *src = NULL;
        const char *used = NULL;
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && !src; i++)
        {
            src = read_file(candidates[i], NULL);
            if (src)
                used = candidates[i];
        }
        CHECK(src != NULL, "main.cpp is readable from the test's runfiles");
        if (src)
        {
            printf("    parser source: %s\n", used);

            char flags[32][FLAG_MAX];
            const int n = flags_in_parser(src, flags, 32);
            printf("    %d long option(s) found in the parser\n", n);
            CHECK(n >= 4, "the parser scan found the options (a scan that finds none would pass vacuously)");

            char *help = render_help();
            CHECK(help != NULL, "the help block renders");
            if (help)
            {
                for (int i = 0; i < n; i++)
                {
                    /* The flag must appear as a LISTED OPTION, not merely somewhere in the
                     * text. Matching the bare string was not enough: the explanatory paragraph
                     * mentions --update-check, so reverting the option line still satisfied a
                     * substring search while the flag was, in fact, undocumented. Option lines
                     * start with exactly two spaces, so the needle is "\n  <flag>". */
                    char needle[FLAG_MAX + 8];
                    snprintf(needle, sizeof(needle), "\n  %s", flags[i]);

                    char msg[160];
                    snprintf(msg, sizeof(msg), "help lists %s as an option", flags[i]);
                    CHECK(contains(help, needle), msg);
                }
                free(help);
            }
            free(src);
        }
    }

    printf("\n── the options that are not obvious say what they are for ──\n");
    {
        char *help = render_help();
        if (help)
        {
            /* Covered by the loop above, but named here because these are the ones that were
             * missing for a release: a bare mention would not have been enough to help the
             * user who could not find them. */
            CHECK(contains(help, "\n  --update-check"), "the update diagnostic is listed as an option");
            CHECK(contains(help, "\n  --current <ver>"), "with the version override a dev build needs");
            CHECK(contains(help, "\n  --pre"), "and the channel it can be asked about");
            CHECK(contains(help, "\n  --sandbox "), "the sandbox override is listed as an option");
            free(help);
        }
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
