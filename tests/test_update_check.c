/* test_update_check: edc_update_check() driven against a local TLS server.
 *
 * The selection rules are already covered offline by test_update.c; what cannot be covered
 * there is the fetch -- building the request, accumulating the body, deciding what to do with
 * a status, a timeout, or a body that will not stop. That is what this file exercises.
 *
 * Reaching GitHub would make this a live test (network, rate limits, and a result that changes
 * whenever someone publishes), so the server is the one-connection TLS stub the endpoint-probe
 * tests already use: a real handshake over loopback with a certificate generated per run, and
 * the response bytes chosen per case.
 *
 * The cases are chosen so that each one would be a silent bug in production:
 *   - a 200 with a valid list -> an offer, and the tag/asset must be the ones the picker chose;
 *   - a 404 -> no offer, no crash;
 *   - silence -> the deadline fires and the call returns rather than hanging the launch;
 *   - a body larger than the cap -> no offer, and the transfer is not accumulated without end;
 *   - malformed JSON -> no offer.
 * Every one of those must be a false return and nothing else, because the caller treats false
 * as "say nothing".
 */

#include "edc_update.h"

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

static int g_failed = 0;
static int g_passed = 0;

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

/* One release for this platform, as the API returns it. The asset name must carry the tag --
 * the picker skips a release whose assets do not include the archive for this build. */
static char *releases_body(const char *tag)
{
    return sdscatprintf(sdsempty(),
                        "[{\"tag_name\":\"%s\",\"draft\":false,\"prerelease\":false,\"assets\":"
                        "[{\"name\":\"%s_%s.tar.gz\"}]}]",
                        tag, edc_update_target(), tag);
}

/* A complete HTTP/1.1 response with a JSON body. Connection: close so the client reads to EOF
 * rather than waiting on a keep-alive it will never reuse. */
static char *http_response(int status, const char *reason, const char *body)
{
    return sdscatprintf(sdsempty(),
                        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n%s",
                        status, reason, strlen(body), body);
}

static void free_body(char *p)
{
    sdsfree(p);
}

/* Run one case against a server that answers |raw| (NULL = handshake then silence). */
static bool run_case(const char *raw, const char *current, bool allow_pre, size_t max_bytes, int timeout_ms,
                     sds *tag, sds *asset)
{
    struct tls_stub s;
    if (tls_stub_start(&s, g_cert, g_key, raw) != 0)
    {
        printf("    (skipped: cannot start the TLS server)\n");
        return false;
    }
    sds url = sdscatprintf(sdsempty(), "https://localhost:%d/repos/o/r/releases", s.port);
    const bool got = edc_update_check(g_cert, url, current, allow_pre, max_bytes, timeout_ms, tag, asset);
    sdsfree(url);
    tls_stub_stop(&s);
    return got;
}

static void test_offers_a_newer_release(void)
{
    printf("\n── a newer release on offer ──\n");
    char *body = releases_body("v9.9.9");
    char *raw = http_response(200, "OK", body);
    sds tag = NULL, asset = NULL;
    const bool got = run_case(raw, "1.0.0", false, 1 << 20, 5000, &tag, &asset);
    CHECK(got, "a 200 with a newer release returns true");
    CHECK(tag && strcmp(tag, "v9.9.9") == 0, "the tag comes back");
    CHECK(asset && strstr(asset, edc_update_target()) != NULL, "the asset names this platform");
    CHECK(asset && strstr(asset, "v9.9.9") != NULL, "the asset names the chosen tag");
    if (tag)
        sdsfree(tag);
    if (asset)
        sdsfree(asset);
    free_body(raw);
    free_body(body);
}

static void test_current_version_is_not_an_offer(void)
{
    printf("\n── the running version is not an update ──\n");
    char *body = releases_body("v1.0.0");
    char *raw = http_response(200, "OK", body);
    sds tag = NULL, asset = NULL;
    const bool got = run_case(raw, "1.0.0", false, 1 << 20, 5000, &tag, &asset);
    CHECK(!got, "the same version returns false");
    CHECK(tag == NULL && asset == NULL, "and nothing is filled in");
    free_body(raw);
    free_body(body);
}

static void test_prerelease_policy(void)
{
    printf("\n── pre-release policy ──\n");
    char *body = sdscatprintf(sdsempty(),
                              "[{\"tag_name\":\"v9.9.9-rc.1\",\"draft\":false,\"prerelease\":true,\"assets\":"
                              "[{\"name\":\"%s_v9.9.9-rc.1.tar.gz\"}]}]",
                              edc_update_target());
    char *raw = http_response(200, "OK", body);

    sds tag = NULL, asset = NULL;
    CHECK(!run_case(raw, "1.0.0", false, 1 << 20, 5000, &tag, &asset),
          "a pre-release is not offered on the stable channel");
    CHECK(tag == NULL, "and no tag is filled in");
    if (tag)
        sdsfree(tag);
    if (asset)
        sdsfree(asset);

    tag = NULL;
    asset = NULL;
    /* The server serves one connection, so this case needs its own. */
    char *raw2 = http_response(200, "OK", body);
    CHECK(run_case(raw2, "1.0.0", true, 1 << 20, 5000, &tag, &asset),
          "a pre-release IS offered when pre-releases are allowed");
    CHECK(tag && strcmp(tag, "v9.9.9-rc.1") == 0, "and the tag is the pre-release");
    if (tag)
        sdsfree(tag);
    if (asset)
        sdsfree(asset);

    free_body(raw);
    free_body(raw2);
    free_body(body);
}

static void test_http_error_offers_nothing(void)
{
    printf("\n── a non-200 answer ──\n");
    char *raw = http_response(403, "Forbidden", "{\"message\":\"rate limit\"}");
    sds tag = NULL, asset = NULL;
    const bool got = run_case(raw, "1.0.0", false, 1 << 20, 5000, &tag, &asset);
    CHECK(!got, "a 403 (the unauthenticated rate limit) offers nothing");
    CHECK(tag == NULL, "and fills in nothing");
    free_body(raw);
}

static void test_malformed_body_offers_nothing(void)
{
    printf("\n── a body that is not the list ──\n");
    char *raw = http_response(200, "OK", "not json at all");
    sds tag = NULL, asset = NULL;
    CHECK(!run_case(raw, "1.0.0", false, 1 << 20, 5000, &tag, &asset), "garbage offers nothing");
    free_body(raw);
}

static void test_oversized_body_offers_nothing(void)
{
    printf("\n── a body past the cap ──\n");
    /* A valid list, but the cap is set below its length: the point is that the limit is
     * enforced on the way in rather than after the fact. */
    char *body = releases_body("v9.9.9");
    char *raw = http_response(200, "OK", body);
    sds tag = NULL, asset = NULL;
    const bool got = run_case(raw, "1.0.0", false, 16, 5000, &tag, &asset);
    CHECK(!got, "a body larger than the cap offers nothing");
    CHECK(tag == NULL, "and fills in nothing");
    free_body(raw);
    free_body(body);
}

/* A response bigger than one TLS record.
 *
 * This exists because the stub used to write the response with a single mbedtls_ssl_write,
 * which returns at most one record (16 KiB) and silently drops the rest. A real release list
 * is ~37 KiB, so serving one produced a body that stopped mid-JSON; the parser rejected it and
 * the check reported "no update available", which is indistinguishable from an up-to-date
 * install. Nothing failed, which is the worst kind of test failure.
 *
 * The case below pads a valid list past 16 KiB and expects it to still parse. If the stub
 * regresses to a single write, the JSON is truncated and this fails.
 */
static void test_response_larger_than_one_tls_record(void)
{
    printf("\n── a response spanning several TLS records ──\n");

    /* A list whose single release carries enough assets to push the body well past 16 KiB.
     * The extra assets are for other platforms, so the picker ignores them; what matters is
     * the byte count. */
    sds json = sdscatprintf(sdsempty(),
                            "[{\"tag_name\":\"v9.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":["
                            "{\"name\":\"%s_v9.9.9.tar.gz\"}",
                            edc_update_target());
    for (int i = 0; i < 400; i++)
        json = sdscatprintf(json, ",{\"name\":\"unrelated-platform-%03d-aaaaaaaaaaaaaaaaaaaa.tar.gz\"}", i);
    json = sdscat(json, "]}]");
    printf("    body: %zu bytes (one TLS record is 16 KiB)\n", sdslen(json));
    CHECK(sdslen(json) > 16 * 1024, "the body really is larger than one record");

    char *raw = http_response(200, "OK", json);
    sds tag = NULL, asset = NULL;
    const bool got = run_case(raw, "1.0.0", false, 1 << 20, 8000, &tag, &asset);
    CHECK(got, "a multi-record response is delivered whole and parses");
    CHECK(tag && strcmp(tag, "v9.9.9") == 0, "and the release is found");

    if (tag)
        sdsfree(tag);
    if (asset)
        sdsfree(asset);
    free_body(raw);
    sdsfree(json);
}

static void test_silence_times_out(void)
{
    printf("\n── the endpoint says nothing ──\n");
    sds tag = NULL, asset = NULL;
    const bool got = run_case(NULL, "1.0.0", false, 1 << 20, 300, &tag, &asset);
    CHECK(!got, "silence returns false rather than hanging");
    CHECK(tag == NULL, "and fills in nothing");
}

static void test_nothing_listening(void)
{
    printf("\n── nothing is listening ──\n");
    /* Take a port from the kernel and give it back: nothing is listening there now. */
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
        return;
    }
    close(fd);

    sds url = sdscatprintf(sdsempty(), "https://localhost:%d/repos/o/r/releases", ntohs(addr.sin_port));
    sds tag = NULL, asset = NULL;
    const bool got = edc_update_check(g_cert, url, "1.0.0", false, 1 << 20, 2000, &tag, &asset);
    CHECK(!got, "a refused connection returns false");
    CHECK(tag == NULL, "and fills in nothing");
    sdsfree(url);
}

static void test_degenerate_arguments(void)
{
    printf("\n── arguments ──\n");
    sds tag = (sds)"sentinel", asset = (sds)"sentinel";
    CHECK(!edc_update_check(g_cert, NULL, "1.0.0", false, 1 << 20, 100, &tag, &asset),
          "a missing URL returns false");
    CHECK(tag == NULL && asset == NULL, "and clears the outputs");
    tag = (sds)"sentinel";
    asset = (sds)"sentinel";
    CHECK(!edc_update_check(g_cert, "", "1.0.0", false, 1 << 20, 100, &tag, &asset), "an empty URL returns false");
    CHECK(tag == NULL && asset == NULL, "and clears the outputs");
    tag = (sds)"sentinel";
    asset = (sds)"sentinel";
    CHECK(!edc_update_check(g_cert, "https://localhost:1/x", NULL, false, 1 << 20, 100, &tag, &asset),
          "a missing current version returns false");
}

int main(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_update_check_%ld", (tmp && *tmp) ? tmp : "/tmp", (long)getpid());
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

    test_offers_a_newer_release();
    test_current_version_is_not_an_offer();
    test_prerelease_policy();
    test_http_error_offers_nothing();
    test_malformed_body_offers_nothing();
    test_oversized_body_offers_nothing();
    test_response_larger_than_one_tls_record();
    test_silence_times_out();
    test_nothing_listening();
    test_degenerate_arguments();

    test_rm_rf(g_dir);
    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
