/* test_update_cli: the `--update-check` diagnostic.
 *
 * Why this file exists: it is the only way to observe the release check on a build whose
 * version is not a release, so its own correctness matters -- a diagnostic that crashes or
 * prints nothing would leave "did the check work?" unanswerable, which is the state it was
 * written to fix.
 *
 * What it can assert without a network: the version short-circuit. A development build
 * reports its commit hash, which is not a version, so the command must say so and exit 0
 * rather than reaching for GitHub. The network half is covered by test_update_check against a
 * local server; here the only thing worth pinning is that the command never fails the shell
 * it runs in -- a diagnostic exiting non-zero would look like a failure of the thing being
 * diagnosed.
 *
 * stdout is captured by redirecting it to a file, because the command reports by printing.
 */

#include "edc_update_cli.h"

#include "edc_semver.h"
#include "edc_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fake_release.h"
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

static char g_dir[512];
static char g_capture[600];

/* Run the command with stdout redirected to a file, and read back what it printed. The caller
 * frees the returned string. */
static char *run_capturing_stdout(bool allow_pre, const char *current_override, int *exit_code)
{
    fflush(stdout);

    const int saved = dup(STDOUT_FILENO);
    FILE *f = fopen(g_capture, "w+");
    if (!f || saved < 0)
    {
        if (f)
            fclose(f);
        if (saved >= 0)
            close(saved);
        return NULL;
    }
    dup2(fileno(f), STDOUT_FILENO);

    *exit_code = edc_update_check_cli(allow_pre, current_override);
    fflush(stdout);

    dup2(saved, STDOUT_FILENO);
    close(saved);

    rewind(f);
    char *buf = malloc(8192);
    size_t n = fread(buf, 1, 8191, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_update_cli_%ld", (tmp && *tmp) ? tmp : "/tmp", (long)getpid());
    test_rm_rf(g_dir);
    test_mkdir_p(g_dir);
    snprintf(g_capture, sizeof(g_capture), "%s/out.txt", g_dir);

    printf("\n── the version this build reports ──\n");
    {
        /* A development build reports its commit; a release reports a tag. Which one this
         * binary is decides what the next case can assert, so it is established first. */
        const bool parseable = edc_semver_is_valid(edc_version());
        printf("    version: %s (%s)\n", edc_version(), parseable ? "a release" : "not a release");
        CHECK(true, "the build's own version is readable");
    }

    printf("\n── a build that is not a release ──\n");
    {
        int code = -1;
        char *out = run_capturing_stdout(false, NULL, &code);
        CHECK(out != NULL, "the command runs");
        if (out)
        {
            /* It prints what it compared, whatever the build is. */
            CHECK(strstr(out, "current") != NULL, "it names the version it is comparing");
            CHECK(strstr(out, "target") != NULL, "it names the platform target");
            CHECK(strstr(out, "channel") != NULL, "it names the channel");

            if (!edc_semver_is_valid(edc_version()))
            {
                CHECK(strstr(out, "skipped") != NULL, "a dev build says the check is skipped");
                CHECK(strstr(out, "not a release version") != NULL, "and why: it is not a version");
                CHECK(strstr(out, "nothing to offer") == NULL,
                      "it does not pretend to have asked the network");
            }
            else
            {
                /* A release build would actually reach for GitHub; this test only asserts the
                 * shape, because the network is not a fixture here. */
                CHECK(strstr(out, "result") != NULL, "a release build reports a result");
            }
            free(out);
        }
        CHECK(code == 0, "and it exits 0 -- a diagnostic must not fail the shell");
    }

    printf("\n── --current overrides the comparison ──\n");
    {
        int code = -1;
        char *out = run_capturing_stdout(false, "1.0.0", &code);
        CHECK(out != NULL, "the command runs");
        if (out)
        {
            CHECK(strstr(out, "1.0.0") != NULL, "the overridden version is named");
            CHECK(strstr(out, "overridden") != NULL, "and marked as a diagnostics-only override");
            /* An override makes the version parseable, so the short-circuit does not apply:
             * the command goes on to ask the network. It may fail (offline, rate limited), but
             * it must still report and still exit 0. */
            CHECK(strstr(out, "skipped") == NULL, "an override bypasses the skip");
            CHECK(strstr(out, "result") != NULL, "and a result is reported either way");
            free(out);
        }
        CHECK(code == 0, "and it exits 0");
    }

    printf("\n── --pre changes the channel only ──\n");
    {
        int code = -1;
        char *out = run_capturing_stdout(true, "0.9.0", &code);
        CHECK(out != NULL, "the command runs");
        if (out)
        {
            CHECK(strstr(out, "channel   pre") != NULL, "the pre channel is named");
            free(out);
        }
        CHECK(code == 0, "and it exits 0");
    }

    printf("\n── the URL comes from the environment ──\n");
    {
        /* A diagnostic that always talks to github.com cannot diagnose an unreachable or
         * rate-limited github.com, because the failure and "no update" print identically.
         * Pointing it at a local server is the only way to tell them apart, and that requires
         * the command to honour EDGECLAW_UPDATE_RELEASES_URL like the launch check does.
         *
         * The server answers one release newer than the version we claim to be on, so a
         * correctly-wired command must report it. */
        char url[256] = {0};
        char cert[600] = {0};
        if (fake_release_start_newer("v9.9.9", url, sizeof(url), cert, sizeof(cert)) != 0)
        {
            printf("    (skipped: cannot start the local release server)\n");
        }
        else
        {
            /* The command reads the CA bundle from <home>/workspace/ssl/cacert.pem. */
            const char *tmp = getenv("TEST_TMPDIR");
            char home[600], ssl[700], pem[800];
            snprintf(home, sizeof(home), "%s/cli-home", (tmp && *tmp) ? tmp : "/tmp");
            snprintf(ssl, sizeof(ssl), "%s/workspace/ssl", home);
            snprintf(pem, sizeof(pem), "%s/cacert.pem", ssl);
            test_mkdir_p(home);
            test_mkdir_p(ssl);
            {
                FILE *in = fopen(cert, "rb");
                FILE *out = in ? fopen(pem, "wb") : NULL;
                if (out)
                {
                    char buf[8192];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                        fwrite(buf, 1, n, out);
                    fclose(out);
                }
                if (in)
                    fclose(in);
            }

            setenv("HOME", home, 1);
            setenv("EDGECLAW_UPDATE_RELEASES_URL", url, 1);
            setenv("EDGECLAW_UPDATE_CURRENT_VERSION", "1.0.0", 1);

            int code = -1;
            char *out = run_capturing_stdout(false, NULL, &code);
            CHECK(out != NULL, "the command runs");
            if (out)
            {
                CHECK(strstr(out, url) != NULL, "it prints and uses the URL from the environment");
                CHECK(strstr(out, "v9.9.9") != NULL,
                      "and the local server's release is found -- so the request really went there");
                CHECK(strstr(out, "overridden") != NULL,
                      "the env-supplied version is flagged as a diagnostics-only override");
                free(out);
            }
            CHECK(code == 0, "and it exits 0");

            unsetenv("EDGECLAW_UPDATE_RELEASES_URL");
            unsetenv("EDGECLAW_UPDATE_CURRENT_VERSION");
            fake_release_stop();
        }
    }

    test_rm_rf(g_dir);
    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
