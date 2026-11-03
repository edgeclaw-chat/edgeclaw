/* test_endpoint_probe: every sentence edc_endpoint_probe() can print, exercised offline.
 *
 * The probe is the "Test" button on the first-run page: it fires one small chat request at
 * the configured base URL and reports whether the endpoint ANSWERED. Reaching a real
 * provider would make this a live test (needs a key, needs the network, fails on a plane),
 * so instead this file stands up its own TLS server on 127.0.0.1 and drives the probe
 * against it:
 *
 *   - the server's certificate is generated at runtime with mbedtls (self-signed, CN=
 *     localhost) and handed to the probe as its cacert, so the TLS verification path runs
 *     for real instead of being disabled -- a broken verify would show up here;
 *   - the response the server sends decides which branch is taken: 200 and 401 both mean
 *     "answered" (the probe's whole point is that a 401 still proves the URL and path are
 *     right), while silence exercises the timeout, and not listening at all exercises the
 *     transport failure.
 *
 * What is asserted is the MESSAGE, not just the bool: the probe's return value cannot tell
 * "wrong URL" from "wrong key", and the message is what the user reads, so that is what the
 * test pins down.
 *
 * No network, no key, no /etc/hosts edit: everything is on the loopback interface.
 */

#include "edc_endpoint_probe.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "uv.h"

#include "tls_stub.h"

#include <inttypes.h> /* PRId64 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

static char g_dir[512];
/* The certificate and the one-connection TLS server live in tests/tls_stub.h: the LLM
 * actor's response tests need exactly the same machinery, and a second copy of it here
 * would drift. See that header for why a stub is unavoidable (every client in this tree
 * insists on a real handshake) and how `raw_response` selects silence.
 */
static char g_cert[600];
static char g_key[600];

/* The two canned answers these cases need. */
static char g_resp_200[512];
static char g_resp_401[512];

/* ── cases ─────────────────────────────────────────────────────────────────── */

/* The required-buffer guard. Both of these used to reach snprintf() and were undefined
 * behaviour; the probe now refuses before touching anything. */
static void test_buffer_required(void)
{
    printf("\n── out / cap are required ──\n");
    CHECK(!edc_endpoint_probe(NULL, "https://localhost:1/x", "k", 100, NULL, 64),
          "a NULL out buffer returns false instead of writing through it");
    {
        char out[32];
        out[0] = 'x';
        CHECK(!edc_endpoint_probe(NULL, "https://localhost:1/x", "k", 100, out, 0),
              "cap == 0 returns false");
        CHECK(out[0] == 'x', "and does not touch the buffer");
    }
}

static void test_bad_url(void)
{
    printf("\n── no URL / bad URL ──\n");
    char out[256];

    CHECK(!edc_endpoint_probe(NULL, NULL, "k", 100, out, sizeof(out)), "NULL url returns false");
    CHECK(strcmp(out, "no base URL") == 0, "NULL url says \"no base URL\"");

    CHECK(!edc_endpoint_probe(NULL, "", "k", 100, out, sizeof(out)), "empty url returns false");
    CHECK(strcmp(out, "no base URL") == 0, "empty url says \"no base URL\"");

    /* No scheme: yuarel refuses to parse it, and the probe reports the URL rather than
     * pretending the network failed. */
    CHECK(!edc_endpoint_probe(NULL, "localhost:1234/chat", "k", 100, out, sizeof(out)),
          "a URL without a scheme returns false");
    CHECK(strcmp(out, "bad URL") == 0, "a URL without a scheme says \"bad URL\"");
}

/* 200 and 401 both mean "the endpoint answered" -- that is the probe's entire contract, and
 * the difference between them is only the advice printed. */
static void test_answered(void)
{
    printf("\n── the endpoint answers ──\n");

    {
        struct tls_stub s;
        char out[256];
        char url[128];
        if (tls_stub_start(&s, g_cert, g_key, g_resp_200) != 0)
        {
            printf("    (skipped: cannot start the TLS server)\n");
            return;
        }
        snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", s.port);
        out[0] = '\0';
        const bool ok = edc_endpoint_probe(g_cert, url, "sk-test", 5000, out, sizeof(out));
        printf("    probe said: %s\n", out);
        CHECK(ok, "HTTP 200 counts as answered");
        CHECK(strcmp(out, "reached — HTTP 200 (OK)") == 0, "200 reports the status and (OK)");
        CHECK(s.served == 2, "the server saw a handshake and a request");
        tls_stub_stop(&s);
    }

    {
        struct tls_stub s;
        char out[256];
        char url[128];
        if (tls_stub_start(&s, g_cert, g_key, g_resp_401) != 0)
        {
            printf("    (skipped: cannot start the TLS server)\n");
            return;
        }
        snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", s.port);
        out[0] = '\0';
        const bool ok = edc_endpoint_probe(g_cert, url, "sk-wrong", 5000, out, sizeof(out));
        printf("    probe said: %s\n", out);
        CHECK(ok, "HTTP 401 ALSO counts as answered (the URL is proven right)");
        CHECK(strcmp(out, "reached — HTTP 401 (check key / model)") == 0,
              "401 points at the key/model instead of the URL");
        tls_stub_stop(&s);
    }
}

static void test_timeout(void)
{
    printf("\n── the endpoint says nothing ──\n");
    struct tls_stub s;
    char out[256];
    char url[128];
    if (tls_stub_start(&s, g_cert, g_key, NULL) != 0)
    {
        printf("    (skipped: cannot start the TLS server)\n");
        return;
    }
    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", s.port);
    out[0] = '\0';
    const bool ok = edc_endpoint_probe(g_cert, url, "sk-test", 300, out, sizeof(out));
    printf("    probe said: %s\n", out);
    CHECK(!ok, "silence returns false");
    CHECK(strcmp(out, "no response (timed out)") == 0, "silence reports the timeout");
    CHECK(s.served == 1, "the handshake had completed -- so this is a timeout, not a TLS failure");
    tls_stub_stop(&s);
}

static void test_connection_failed(void)
{
    printf("\n── nothing is listening ──\n");
    char out[256];
    char url[128];

    /* Take a port from the kernel, then give it back: nothing is listening on it now. */
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

    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", ntohs(addr.sin_port));
    out[0] = '\0';
    const bool ok = edc_endpoint_probe(g_cert, url, "sk-test", 2000, out, sizeof(out));
    printf("    probe said: %s\n", out);
    CHECK(!ok, "a refused connection returns false");
    CHECK(strcmp(out, "connection failed") == 0, "a refused connection says so");
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_endpoint_probe_%" PRId64,
             (tmp && *tmp) ? tmp : "/tmp", (int64_t)getpid());
    snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", g_dir);
    snprintf(g_key, sizeof(g_key), "%s/key.pem", g_dir);

    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "    (warning: could not create %s)\n", g_dir);

    printf("workspace: %s\n", g_dir);

    if (tls_stub_cert(g_cert, g_key) != 0)
    {
        fprintf(stderr, "cannot generate the test certificate; skipping\n");
        return 1;
    }

    /* The two canned answers, built once: a plain body is enough here, because the probe
     * only reads the status line. (The LLM actor's tests use the same stub with bodies that
     * matter.) */
    tls_stub_response(g_resp_200, sizeof(g_resp_200), "200 OK", "{\"ok\":true}");
    tls_stub_response(g_resp_401, sizeof(g_resp_401), "401 Unauthorized", "{\"ok\":false}");

    test_buffer_required();
    test_bad_url();
    test_answered();
    test_timeout();
    test_connection_failed();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", g_dir);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
