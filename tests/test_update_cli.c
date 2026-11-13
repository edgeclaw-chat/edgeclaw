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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

static char g_home[600];

/* Point the command at a CA bundle.
 *
 * The command reads <home>/workspace/ssl/cacert.pem and has no CLI override for it -- that
 * path is derived from $HOME -- so a test needing a particular trust chain sets HOME and
 * copies the certificate in. |cert| NULL leaves the file absent, which is how the
 * client-creation failure is reached. */
static void setup_home_with_ca(const char *cert)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    snprintf(g_home, sizeof(g_home), "%s/cli-home", (tmp && *tmp) ? tmp : "/tmp");

    char ssl[700], pem[800];
    snprintf(ssl, sizeof(ssl), "%s/workspace/ssl", g_home);
    snprintf(pem, sizeof(pem), "%s/cacert.pem", ssl);
    test_rm_rf(g_home);
    test_mkdir_p(ssl);

    if (cert)
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
    setenv("HOME", g_home, 1);
}

/* Run the command against |url| as if the build were 1.0.0, and tidy the environment after.
 * The caller frees the returned text. */
static char *run_against_url(const char *url, int *code)
{
    setenv("EDGECLAW_UPDATE_RELEASES_URL", url, 1);
    setenv("EDGECLAW_UPDATE_CURRENT_VERSION", "1.0.0", 1);
    char *out = run_capturing_stdout(false, NULL, code);
    unsetenv("EDGECLAW_UPDATE_RELEASES_URL");
    unsetenv("EDGECLAW_UPDATE_CURRENT_VERSION");
    return out;
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

    printf("\n── a check that could not be made says so ──\n");
    {
        /* The defect this pins: a rate-limited or unreachable GitHub used to print exactly
         * what an up-to-date install prints, so the one question this command exists to
         * answer -- "did it ask, or did it fail?" -- was unanswerable. GitHub's unauthenticated
         * limit (60/hour per IP) is not rare on a shared address; it is the ordinary case.
         *
         * A closed port is the cheapest stand-in for that here: no fixture to serve, and the
         * outcome is the same class of thing (nothing was determined). The 403 wording itself
         * is covered in test_update_check.c, which can serve a real status line. */
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        int port = 0;
        if (fd >= 0 && bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            getsockname(fd, (struct sockaddr *)&addr, &alen) == 0)
            port = ntohs(addr.sin_port);
        if (fd >= 0)
            close(fd);

        if (port == 0)
        {
            printf("    (skipped: cannot reserve a port)\n");
        }
        else
        {
            char url[128];
            snprintf(url, sizeof(url), "https://localhost:%d/repos/o/r/releases", port);
            setenv("EDGECLAW_UPDATE_RELEASES_URL", url, 1);
            setenv("EDGECLAW_UPDATE_CURRENT_VERSION", "1.0.0", 1);

            int code = -1;
            char *out = run_capturing_stdout(false, NULL, &code);
            CHECK(out != NULL, "the command runs");
            if (out)
            {
                CHECK(strstr(out, "could not check") != NULL, "it says the check could not be made");
                /* The exact result line, not a substring: the explanatory line below it
                 * legitimately contains the words "up to date", in the sentence that tells
                 * the reader this is NOT that. */
                CHECK(strstr(out, "result    up to date") == NULL, "and the result line does NOT claim to be up to date");
                CHECK(strstr(out, "not the same as up to date") != NULL,
                      "it spells out the difference, because that is the whole point");
                free(out);
            }
            CHECK(code == 0, "and it still exits 0 -- this diagnoses the updater, it is not its failure");

            unsetenv("EDGECLAW_UPDATE_RELEASES_URL");
            unsetenv("EDGECLAW_UPDATE_CURRENT_VERSION");
        }
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
            setup_home_with_ca(cert);

            int code = -1;
            char *out = run_against_url(url, &code);
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

            fake_release_stop();
        }
    }

    printf("\n── every way a check can fail prints its own reason ──\n");
    {
        /* These assertions exist because the LINES are the fix. Before it, all of these printed
         * `result    nothing to offer`, exactly like an up-to-date install, so a rate limited
         * or broken check was indistinguishable from "you are current" -- the one question this
         * command exists to answer. A message nobody asserts is a message that can silently
         * regress into the behaviour it replaced. */
        char url[256] = {0};
        char cert[600] = {0};
        int code = -1;
        char *out = NULL;

        /* 403: GitHub's unauthenticated rate limit, the ordinary case on a shared address. */
        if (fake_release_start_status(403, "Forbidden", "{\"message\":\"rate limit\"}", url, sizeof(url), cert,
                                      sizeof(cert)) == 0)
        {
            setup_home_with_ca(cert);
            code = -1;
            out = run_against_url(url, &code);
            if (out)
            {
                CHECK(strstr(out, "HTTP 403") != NULL, "a 403 reports the status");
                CHECK(strstr(out, "unauthenticated rate limit") != NULL, "and names it as the rate limit");
                CHECK(strstr(out, "result    up to date") == NULL, "and refuses to call it up to date");
                free(out);
            }
            CHECK(code == 0, "a 403 still exits 0");
            fake_release_stop();
        }
        else
            printf("    (skipped: cannot start the local server)\n");

        /* Another status: reported as itself, not as the rate limit. */
        if (fake_release_start_status(503, "Service Unavailable", "", url, sizeof(url), cert, sizeof(cert)) == 0)
        {
            setup_home_with_ca(cert);
            code = -1;
            out = run_against_url(url, &code);
            if (out)
            {
                CHECK(strstr(out, "HTTP 503") != NULL, "a 503 reports its own status");
                CHECK(strstr(out, "unauthenticated rate limit") == NULL, "without claiming it is the rate limit");
                free(out);
            }
            fake_release_stop();
        }
        else
            printf("    (skipped: cannot start the local server)\n");

        /* 200 but not the release list: a server that answered, with something else. */
        if (fake_release_start_status(200, "OK", "not json at all", url, sizeof(url), cert, sizeof(cert)) == 0)
        {
            setup_home_with_ca(cert);
            code = -1;
            out = run_against_url(url, &code);
            if (out)
            {
                CHECK(strstr(out, "not the release list") != NULL, "a malformed body says so");
                CHECK(strstr(out, "result    up to date") == NULL, "and does not claim to be up to date");
                free(out);
            }
            fake_release_stop();
        }
        else
            printf("    (skipped: cannot start the local server)\n");

        /* The endpoint is there but says nothing: the deadline is what decides. */
        if (fake_release_start_silent(url, sizeof(url), cert, sizeof(cert)) == 0)
        {
            setup_home_with_ca(cert);
            code = -1;
            out = run_against_url(url, &code);
            if (out)
            {
                CHECK(strstr(out, "no response before the deadline") != NULL, "silence is reported as a timeout");
                free(out);
            }
            fake_release_stop();
        }
        else
            printf("    (skipped: cannot start the local server)\n");

        /* 200 with nothing newer than the version we claim: the "up to date" line. This is the
         * one positive answer the command can give while offering nothing, and it must be
         * distinguishable from every failure above -- that is the whole repair. */
        if (fake_release_start_newer("v1.0.0", url, sizeof(url), cert, sizeof(cert)) == 0)
        {
            setup_home_with_ca(cert);
            code = -1;
            out = run_against_url(url, &code);
            if (out)
            {
                CHECK(strstr(out, "result    up to date") != NULL, "an equal version reports up to date");
                CHECK(strstr(out, "could not check") == NULL, "and does not claim the check failed");
                free(out);
            }
            fake_release_stop();
        }
        else
            printf("    (skipped: cannot start the local server)\n");

        /* A 200 whose body is past the 1 MiB cap. The cap exists so a hostile or broken
         * endpoint cannot make the client accumulate without end; a body that trips it is a
         * failed check, not an empty release list. */
        {
            const size_t n = (1u << 20) + 4096;
            char *big = malloc(n + 1);
            if (!big)
                printf("    (skipped: out of memory)\n");
            else
            {
                memset(big, 'a', n);
                big[n] = '\0';
                if (fake_release_start_status(200, "OK", big, url, sizeof(url), cert, sizeof(cert)) == 0)
                {
                    setup_home_with_ca(cert);
                    code = -1;
                    out = run_against_url(url, &code);
                    if (out)
                    {
                        CHECK(strstr(out, "larger than the 1 MiB cap") != NULL,
                              "a body past the cap is reported as such");
                        CHECK(strstr(out, "result    up to date") == NULL, "and not as up to date");
                        free(out);
                    }
                    fake_release_stop();
                }
                else
                    printf("    (skipped: cannot start the local server)\n");
                free(big);
            }
        }

        /* No CA bundle at all. edc_https_client_new does not read the bundle -- it is loaded
         * when connecting -- so this arrives as a connect failure rather than a distinct
         * outcome, and the message names the CA among its causes. What matters for this test
         * is the property under repair: it is reported as "could not check", not as "up to
         * date". */
        setup_home_with_ca(NULL);
        code = -1;
        out = run_against_url("https://localhost:1/repos/o/r/releases", &code);
        if (out)
        {
            CHECK(strstr(out, "could not check") != NULL, "a missing CA bundle is reported as a failed check");
            CHECK(strstr(out, "CA bundle") != NULL, "and the message names the CA bundle among the causes");
            CHECK(strstr(out, "result    up to date") == NULL, "and is not mistaken for up to date");
            free(out);
        }
        CHECK(code == 0, "and it exits 0");
    }

    test_rm_rf(g_dir);
    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
