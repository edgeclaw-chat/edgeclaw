/* test_upgrade_run: the upgrade runner, driven against a local fixture installer.
 *
 * Why this file exists: this module fetches a script, writes it to a temp file, spawns it and
 * reports the outcome -- and had no tests. Everything about it that can be wrong is about
 * what happens AROUND the child: a non-zero exit being reported as success, a hung installer
 * hanging the process, output being lost, or a spawn failure leaking a handle.
 *
 * The network half is the one thing not exercised here: |script_url| points at a local
 * fixture server over TLS (the same stub the endpoint-probe tests use), so the fetch path
 * runs for real without touching GitHub. What the fetched script DOES is chosen per case, so
 * each test breaks exactly one thing:
 *
 *   - a script that exits 0            -> ok
 *   - a script that exits 3 with text  -> not ok, the status and the output are reported
 *   - a script that never finishes     -> the deadline kills it, and that is reported
 *   - an unreachable server            -> not ok, and nothing is spawned
 *
 * The loop is driven by hand (UV_RUN_NOWAIT in a bounded loop) rather than
 * UV_RUN_DEFAULT: a runaway child under the default mode would hang the test binary, which is
 * the same reason edc_actor_runner_free() bounds its drain.
 */

#include "edc_upgrade_run.h"

#include "sds.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "uv.h"

#include "tls_stub.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
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
static char g_cert[600];
static char g_key[600];
static char g_install_dir[600];

/* What the runner reported, filled by the done callback. */
struct outcome
{
    int done;
    bool ok;
    /* Larger than the runner's 4 KiB tail PLUS the status line in front of it: a buffer of
     * exactly the tail size clipped the very marker the cap test looks for. */
    char detail[8192];
};

static void on_done(bool ok, const char *detail, void *ud)
{
    struct outcome *o = ud;
    o->done++;
    o->ok = ok;
    snprintf(o->detail, sizeof(o->detail), "%s", detail ? detail : "");
}

/* Build a complete HTTP response carrying |body|. */
static char *http_ok(const char *body)
{
    return sdscatprintf(sdsempty(),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n%s",
                        strlen(body), body);
}

/* Drive the loop until |o->done| or the deadline. Returns whether it completed. */
static bool pump_until(uv_loop_t *loop, struct outcome *o, int iterations)
{
    for (int i = 0; i < iterations; i++)
    {
        uv_run(loop, UV_RUN_NOWAIT);
        if (o->done)
            return true;
        uv_sleep(10);
    }
    return o->done != 0;
}

/* One case: serve |script| from the fixture server and run the upgrade. */
static void run_case(const char *script, int timeout_ms, int iterations, struct outcome *o)
{
    memset(o, 0, sizeof(*o));

    char *raw = http_ok(script);
    struct tls_stub s;
    if (tls_stub_start(&s, g_cert, g_key, raw) != 0)
    {
        printf("    (skipped: cannot start the TLS server)\n");
        sdsfree(raw);
        return;
    }

    uv_loop_t loop;
    uv_loop_init(&loop);

    sds url = sdscatprintf(sdsempty(), "https://localhost:%d/tools/install.sh", s.port);
    struct edc_upgrade_run *run =
        edc_upgrade_run_start(&loop, g_cert, url, "v9.9.9", g_install_dir, timeout_ms, on_done, o);
    CHECK(run != NULL, "the runner starts");

    if (run)
        pump_until(&loop, o, iterations);

    edc_upgrade_run_stop(run);
    /* Drain the closes the stop above queued, so the loop can be torn down. */
    for (int i = 0; i < 100 && uv_run(&loop, UV_RUN_NOWAIT) != 0; i++)
    {
    }
    uv_loop_close(&loop);

    sdsfree(url);
    tls_stub_stop(&s);
    sdsfree(raw);
}

int main(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_upgrade_run_%ld", (tmp && *tmp) ? tmp : "/tmp", (long)getpid());
    test_rm_rf(g_dir);
    test_mkdir_p(g_dir);
    snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", g_dir);
    snprintf(g_key, sizeof(g_key), "%s/key.pem", g_dir);
    snprintf(g_install_dir, sizeof(g_install_dir), "%s/bin", g_dir);
    test_mkdir_p(g_install_dir);
    if (tls_stub_cert(g_cert, g_key) != 0)
    {
        fprintf(stderr, "cannot generate a certificate -- skipping\n");
        test_rm_rf(g_dir);
        return 0;
    }

    printf("\n── an installer that succeeds ──\n");
    {
        struct outcome o;
        run_case("#!/bin/sh\necho \"installing for $2\"\nexit 0\n", 5000, 800, &o);
        CHECK(o.done == 1, "the callback fires exactly once");
        CHECK(o.ok, "a zero exit is reported as success");
    }

    printf("\n── an installer that fails ──\n");
    {
        struct outcome o;
        run_case("#!/bin/sh\necho 'cannot write /usr/local/bin' >&2\nexit 3\n", 5000, 800, &o);
        CHECK(o.done == 1, "the callback fires exactly once");
        CHECK(!o.ok, "a non-zero exit is reported as failure");
        CHECK(strstr(o.detail, "status 3") != NULL, "the exit status is in the detail");
        CHECK(strstr(o.detail, "cannot write") != NULL, "the installer's stderr reaches the detail");
    }

    printf("\n── the installer's arguments ──\n");
    {
        /* The tag and the directory are what make the upgrade land on the running binary, so
         * the script asserts them rather than succeeding unconditionally. */
        char script[2048];
        snprintf(script, sizeof(script),
                 "#!/bin/sh\n"
                 "case \"$*\" in\n"
                 "  *\"--tag v9.9.9\"*) ;;\n"
                 "  *) echo \"missing --tag: $*\" >&2; exit 9 ;;\n"
                 "esac\n"
                 "case \"$*\" in\n"
                 "  *\"--dir %s\"*) ;;\n"
                 "  *) echo \"missing --dir: $*\" >&2; exit 9 ;;\n"
                 "esac\n"
                 "exit 0\n",
                 g_install_dir);
        struct outcome o;
        run_case(script, 5000, 800, &o);
        CHECK(o.done == 1 && o.ok, "the installer receives --tag and --dir");
        if (!o.ok)
            printf("      detail: %s\n", o.detail);
    }

    printf("\n── an installer that hangs ──\n");
    {
        /* The deadline has to fire: a hung installer must not hold the process. `exec` so the
         * shell is REPLACED by sleep -- otherwise the kill hits the shell while sleep keeps
         * the pipe open, which is the orphan case run_ready_to_report() also defends
         * against. */
        struct outcome o;
        run_case("#!/bin/sh\nexec sleep 60\n", /* timeout_ms */ 300, 800, &o);
        CHECK(o.done == 1, "the callback fires even though the installer never finished");
        CHECK(!o.ok, "a timed-out installer is reported as failure");
        CHECK(strstr(o.detail, "did not finish in time") != NULL || strstr(o.detail, "signal") != NULL,
              "and the detail says why");
    }

    printf("\n── the fetch fails ──\n");
    {
        struct outcome o;
        memset(&o, 0, sizeof(o));

        /* A port the kernel hands out and takes back: nothing is listening there. */
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            getsockname(fd, (struct sockaddr *)&addr, &alen) != 0)
        {
            if (fd >= 0)
                close(fd);
            printf("    (skipped: cannot reserve a port)\n");
        }
        else
        {
            close(fd);
            uv_loop_t loop;
            uv_loop_init(&loop);
            sds url = sdscatprintf(sdsempty(), "https://localhost:%d/tools/install.sh", ntohs(addr.sin_port));
            struct edc_upgrade_run *run =
                edc_upgrade_run_start(&loop, g_cert, url, "v9.9.9", g_install_dir, 2000, on_done, &o);
            CHECK(run != NULL, "the runner starts");
            if (run)
                pump_until(&loop, &o, 800);
            edc_upgrade_run_stop(run);
            for (int i = 0; i < 100 && uv_run(&loop, UV_RUN_NOWAIT) != 0; i++)
            {
            }
            uv_loop_close(&loop);
            sdsfree(url);

            CHECK(o.done == 1, "the callback still fires");
            CHECK(!o.ok, "an unreachable installer URL is a failure");
            CHECK(strstr(o.detail, "cannot fetch") != NULL, "and says the fetch is what failed");
        }
    }

    printf("\n── output past the tail cap ──\n");
    {
        /* The detail keeps the last 4 KiB. A script that prints more than that exercises the
         * trim, and the tail (not the head) has to survive: it is where a failing installer
         * writes its error. */
        /* awk rather than a shell loop: 900 iterations of `[ ]` fork a process each time, and
         * the 5s deadline fired before the script finished -- which then took the timeout
         * path (report without waiting for the pipes) and truncated the tail. The test has to
         * produce a lot of output QUICKLY to exercise the trim rather than the deadline. */
        struct outcome o;
        run_case("#!/bin/sh\n"
                 "echo HEAD_MARKER\n"
                 "awk 'BEGIN{for(i=0;i<900;i++)print \"padding padding padding padding\"}'\n"
                 "echo TAIL_MARKER\n"
                 "exit 1\n",
                 20000, 2000, &o);
        CHECK(o.done == 1, "the callback fires");
        CHECK(!o.ok, "a non-zero exit is a failure");
        CHECK(strstr(o.detail, "TAIL_MARKER") != NULL, "the tail of the output is kept");
        CHECK(strstr(o.detail, "HEAD_MARKER") == NULL, "the head is dropped once the cap is passed");
    }

    printf("\n── killed by a signal, not by the deadline ──\n");
    {
        /* A different path from the timeout: the child dies of its own accord, so the detail
         * names the signal rather than the deadline. */
        struct outcome o;
        run_case("#!/bin/sh\nkill -TERM $$\n", 5000, 800, &o);
        CHECK(o.done == 1, "the callback fires");
        CHECK(!o.ok, "a signalled installer is a failure");
        CHECK(strstr(o.detail, "signal") != NULL, "and the detail names the signal");
        CHECK(strstr(o.detail, "did not finish in time") == NULL, "it is not reported as a timeout");
    }

    printf("\n── stopping while the installer runs ──\n");
    {
        /* stop() has to kill a live child, not wait for it: this is what Esc does. */
        char *raw = http_ok("#!/bin/sh\nexec sleep 30\n");
        struct tls_stub s;
        if (tls_stub_start(&s, g_cert, g_key, raw) != 0)
        {
            printf("    (skipped)\n");
        }
        else
        {
            uv_loop_t loop;
            uv_loop_init(&loop);
            struct outcome o;
            memset(&o, 0, sizeof(o));
            sds url = sdscatprintf(sdsempty(), "https://localhost:%d/tools/install.sh", s.port);
            struct edc_upgrade_run *run =
                edc_upgrade_run_start(&loop, g_cert, url, "v9.9.9", g_install_dir, 60000, on_done, &o);
            CHECK(run != NULL, "the runner starts");

            /* Let it reach the spawned state, then stop. */
            for (int i = 0; i < 300 && o.done == 0; i++)
            {
                uv_run(&loop, UV_RUN_NOWAIT);
                uv_sleep(10);
                if (i == 50)
                    break; /* it should be running by now; stop it */
            }
            edc_upgrade_run_stop(run);
            CHECK(true, "stopping a run whose installer is still alive returns");

            for (int i = 0; i < 300 && uv_run(&loop, UV_RUN_NOWAIT) != 0; i++)
            {
            }
            CHECK(uv_loop_close(&loop) == 0, "the loop closes: the killed child's handles are gone");
            sdsfree(url);
            tls_stub_stop(&s);
        }
        sdsfree(raw);
    }

    printf("\n── degenerate arguments ──\n");
    {
        uv_loop_t loop;
        uv_loop_init(&loop);
        struct outcome o;
        memset(&o, 0, sizeof(o));
        CHECK(edc_upgrade_run_start(NULL, g_cert, "https://x/y", "v1", g_install_dir, 1000, on_done, &o) == NULL,
              "no loop means nothing starts");
        CHECK(edc_upgrade_run_start(&loop, g_cert, NULL, "v1", g_install_dir, 1000, on_done, &o) == NULL,
              "no script URL means nothing starts");
        CHECK(edc_upgrade_run_start(&loop, g_cert, "", "v1", g_install_dir, 1000, on_done, &o) == NULL,
              "an empty script URL means nothing starts");
        CHECK(edc_upgrade_run_start(&loop, g_cert, "https://x/y", NULL, g_install_dir, 1000, on_done, &o) == NULL,
              "no tag means nothing starts");
        CHECK(edc_upgrade_run_start(&loop, g_cert, "https://x/y", "v1", NULL, 1000, on_done, &o) == NULL,
              "no install directory means nothing starts");
        CHECK(edc_upgrade_run_start(&loop, g_cert, "https://x/y", "v1", g_install_dir, 1000, NULL, &o) == NULL,
              "no completion callback means nothing starts");
        edc_upgrade_run_stop(NULL); /* must be a no-op */
        CHECK(true, "stopping NULL is a no-op");
        uv_loop_close(&loop);
    }

    test_rm_rf(g_dir);
    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
