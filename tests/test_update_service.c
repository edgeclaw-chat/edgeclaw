/* test_update_service: the once-per-launch check, on a real loop.
 *
 * Why this file exists: this is the only code that runs on a thread of its own and then posts
 * back into somebody else's loop, and none of it was tested. The things that can go wrong are
 * all about that handover rather than about the check itself (which test_update_check covers):
 *
 *   - the delay is the REQUEST's, not just the report's. A worker that fetched at once and
 *     merely waited to report would still compete with the launch's model request, which is
 *     the whole reason the delay exists. Asserted by watching the server: it must not be
 *     contacted while the timer has not fired;
 *   - the callback runs on the loop thread and runs once;
 *   - "nothing newer" produces no callback at all -- silence is the contract, and a stray
 *     callback would pop a banner for a release the user already has;
 *   - stop() while the worker is still parked on the semaphore joins instead of deadlocking,
 *     and leaves the loop closeable (its handles really are closed, not leaked).
 */

#include "edc_update_service.h"

#include "edc_update.h"
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

struct hits
{
    int count;
    char tag[64];
};

static void on_found(const char *tag, const char *asset, void *ud)
{
    (void)asset;
    struct hits *h = ud;
    h->count++;
    snprintf(h->tag, sizeof(h->tag), "%s", tag ? tag : "");
}

static char *http_ok(const char *body)
{
    return sdscatprintf(sdsempty(),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n%s",
                        strlen(body), body);
}

/* One release for THIS platform, newer than the version the case passes in. The asset name
 * comes from edc_update_target() rather than a literal: a hardcoded platform makes the
 * picker correctly skip the release on every other machine, which looks like a broken check. */
static char *newer_release(void)
{
    return sdscatprintf(sdsempty(),
                        "[{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":"
                        "[{\"name\":\"%s_v9.9.9.tar.gz\"}]}]",
                        edc_update_target());
}

/* Drive the loop for |ms|, stopping early when |h->count| reaches |want|. */
static void pump(uv_loop_t *loop, struct hits *h, int want, int ms)
{
    for (int i = 0; i < ms / 10; i++)
    {
        uv_run(loop, UV_RUN_NOWAIT);
        if (h && want > 0 && h->count >= want)
            return;
        uv_sleep(10);
    }
}

int main(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_update_service_%ld", (tmp && *tmp) ? tmp : "/tmp", (long)getpid());
    test_rm_rf(g_dir);
    test_mkdir_p(g_dir);
    snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", g_dir);
    snprintf(g_key, sizeof(g_key), "%s/key.pem", g_dir);
    if (tls_stub_cert(g_cert, g_key) != 0)
    {
        fprintf(stderr, "cannot generate a certificate -- skipping\n");
        test_rm_rf(g_dir);
        return 0;
    }

    printf("\n── a newer release is reported ──\n");
    {
        char *body = newer_release();
        char *raw = http_ok(body);
        struct tls_stub s;
        if (tls_stub_start(&s, g_cert, g_key, raw) != 0)
        {
            printf("    (skipped: cannot start the TLS server)\n");
        }
        else
        {
            uv_loop_t loop;
            uv_loop_init(&loop);
            struct hits h = {0, {0}};
            sds url = sdscatprintf(sdsempty(), "https://localhost:%d/repos/o/r/releases", s.port);
            struct edc_update_service *svc =
                edc_update_service_start(&loop, g_cert, url, "1.0.0", false, /* delay */ 0, on_found, &h);
            CHECK(svc != NULL, "the service starts");
            pump(&loop, &h, 1, 5000);
            CHECK(h.count == 1, "the callback fires once");
            CHECK(strcmp(h.tag, "v9.9.9") == 0, "and carries the offered tag");
            pump(&loop, &h, 0, 300); /* the check never repeats */
            CHECK(h.count == 1, "and only once -- this is not a timer");

            edc_update_service_stop(svc);
            for (int i = 0; i < 200 && uv_run(&loop, UV_RUN_NOWAIT) != 0; i++)
            {
            }
            CHECK(uv_loop_close(&loop) == 0, "the loop closes: stop() closed its handles");
            sdsfree(url);
            tls_stub_stop(&s);
        }
        sdsfree(raw);
        sdsfree(body);
    }

    printf("\n── nothing newer means no callback ──\n");
    {
        /* The running version is the one published: silence is the contract. */
        char *body = newer_release();
        char *raw = http_ok(body);
        struct tls_stub s;
        if (tls_stub_start(&s, g_cert, g_key, raw) != 0)
        {
            printf("    (skipped)\n");
        }
        else
        {
            uv_loop_t loop;
            uv_loop_init(&loop);
            struct hits h = {0, {0}};
            sds url = sdscatprintf(sdsempty(), "https://localhost:%d/repos/o/r/releases", s.port);
            struct edc_update_service *svc =
                edc_update_service_start(&loop, g_cert, url, "9.9.9", false, 0, on_found, &h);
            CHECK(svc != NULL, "the service starts");
            pump(&loop, &h, 0, 2000);
            CHECK(h.count == 0, "an install that is already current gets no callback");

            edc_update_service_stop(svc);
            for (int i = 0; i < 200 && uv_run(&loop, UV_RUN_NOWAIT) != 0; i++)
            {
            }
            CHECK(uv_loop_close(&loop) == 0, "the loop closes");
            sdsfree(url);
            tls_stub_stop(&s);
        }
        sdsfree(raw);
        sdsfree(body);
    }

    printf("\n── the delay holds the REQUEST, not just the report ──\n");
    {
        /* The server records whether it was contacted. With a delay the worker must still be
         * parked, so the handshake has not happened yet. */
        char *body = newer_release();
        char *raw = http_ok(body);
        struct tls_stub s;
        if (tls_stub_start(&s, g_cert, g_key, raw) != 0)
        {
            printf("    (skipped)\n");
        }
        else
        {
            uv_loop_t loop;
            uv_loop_init(&loop);
            struct hits h = {0, {0}};
            sds url = sdscatprintf(sdsempty(), "https://localhost:%d/repos/o/r/releases", s.port);
            struct edc_update_service *svc = edc_update_service_start(&loop, g_cert, url, "1.0.0", false,
                                                                     /* delay */ 3000, on_found, &h);
            CHECK(svc != NULL, "the service starts");

            pump(&loop, &h, 0, 400); /* well inside the delay */
            CHECK(h.count == 0, "nothing is reported before the delay elapses");
            CHECK(s.served == 0, "and the server was not contacted -- the request itself waits");

            /* Stop while the worker is parked on the semaphore: the join must release it, not
             * deadlock on a timer that will never fire. */
            edc_update_service_stop(svc);
            CHECK(true, "stopping a service whose delay has not elapsed returns");
            for (int i = 0; i < 200 && uv_run(&loop, UV_RUN_NOWAIT) != 0; i++)
            {
            }
            CHECK(uv_loop_close(&loop) == 0, "the loop closes");
            sdsfree(url);
            tls_stub_stop(&s);
        }
        sdsfree(raw);
        sdsfree(body);
    }

    printf("\n── degenerate arguments ──\n");
    {
        uv_loop_t loop;
        uv_loop_init(&loop);
        struct hits h = {0, {0}};
        CHECK(edc_update_service_start(NULL, g_cert, "https://x/y", "1.0.0", false, 0, on_found, &h) == NULL,
              "no loop means nothing starts");
        CHECK(edc_update_service_start(&loop, g_cert, NULL, "1.0.0", false, 0, on_found, &h) == NULL,
              "no URL means nothing starts");
        CHECK(edc_update_service_start(&loop, g_cert, "", "1.0.0", false, 0, on_found, &h) == NULL,
              "an empty URL means nothing starts");
        CHECK(edc_update_service_start(&loop, g_cert, "https://x/y", NULL, false, 0, on_found, &h) == NULL,
              "no current version means nothing starts");
        CHECK(edc_update_service_start(&loop, g_cert, "https://x/y", "1.0.0", false, 0, NULL, &h) == NULL,
              "no callback means nothing starts");
        edc_update_service_stop(NULL);
        CHECK(true, "stopping NULL is a no-op");
        CHECK(uv_loop_close(&loop) == 0, "the loop closes");
    }

    test_rm_rf(g_dir);
    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
