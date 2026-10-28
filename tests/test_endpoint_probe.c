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
static char g_cert[600];
static char g_key[600];

/* ── the certificate ───────────────────────────────────────────────────────────
 *
 * Self-signed with CN=localhost. mbedtls falls back to the CN when a certificate carries
 * no subjectAltName (x509_crt.c, x509_crt_verify_name), so no SAN extension is needed for
 * the "localhost" in the probe's URL to match. Generated per run rather than checked in:
 * a committed test key is a key, however scoped.
 */
static int write_cert_and_key(void)
{
    int rc = -1;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    unsigned char buf[8192];

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, NULL, 0) != 0)
        goto out;

    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0)
        goto out;
    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &drbg, 2048, 65537) != 0)
        goto out;

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=localhost,O=EdgeClaw") != 0)
        goto out;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=localhost,O=EdgeClaw") != 0)
        goto out;

    /* Wide, fixed validity so the test cannot rot. */
    if (mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20450101000000") != 0)
        goto out;

    {
        unsigned char serial[1] = {1};
        if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0)
            goto out;
    }

    /* CA:TRUE + keyCertSign: the probe parses this file as its trust anchor, and mbedtls
     * requires the signing certificate to be allowed to sign. */
    if (mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0) != 0)
        goto out;
    if (mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                                      MBEDTLS_X509_KU_KEY_CERT_SIGN) != 0)
        goto out;
    if (mbedtls_x509write_crt_set_subject_key_identifier(&crt) != 0)
        goto out;
    if (mbedtls_x509write_crt_set_authority_key_identifier(&crt) != 0)
        goto out;

    memset(buf, 0, sizeof(buf));
    if (mbedtls_x509write_crt_pem(&crt, buf, sizeof(buf), mbedtls_ctr_drbg_random, &drbg) != 0)
        goto out;
    {
        FILE *f = fopen(g_cert, "wb");
        if (!f)
            goto out;
        const int ok = fputs((const char *)buf, f) >= 0;
        fclose(f);
        if (!ok)
            goto out;
    }

    memset(buf, 0, sizeof(buf));
    if (mbedtls_pk_write_key_pem(&key, buf, sizeof(buf)) != 0)
        goto out;
    {
        FILE *f = fopen(g_key, "wb");
        if (!f)
            goto out;
        const int ok = fputs((const char *)buf, f) >= 0;
        fclose(f);
        if (!ok)
            goto out;
    }

    rc = 0;
out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return rc;
}

/* ── the server ────────────────────────────────────────────────────────────────
 *
 * One connection, one canned answer. Which answer is the parameter, so the same code path
 * covers 200 / 401 / silence.
 */
enum resp_mode
{
    RESP_200,
    RESP_401,
    RESP_SILENT
};

struct tls_server
{
    int listen_fd;
    int port;
    enum resp_mode mode;
    uv_thread_t thread;
    int served; /* set by the thread: did a client actually get as far as a handshake? */
};

/* Serves one connection, then exits. Forward-declared: the body sits below, next to the
 * response switch it implements. */
static void server_run(struct tls_server *s, int client_fd);

static void server_thread(void *arg)
{
    struct tls_server *s = arg;
    const int client_fd = accept(s->listen_fd, NULL, NULL);
    if (client_fd >= 0)
    {
        server_run(s, client_fd);
        close(client_fd);
    }
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    const int fd = *(int *)ctx;
    const ssize_t n = send(fd, buf, len, 0);
    if (n < 0)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    const int fd = *(int *)ctx;
    const ssize_t n = recv(fd, buf, len, 0);
    if (n < 0)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (n == 0)
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return (int)n;
}

static void server_run(struct tls_server *s, int client_fd)
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    int ret;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, NULL, 0) != 0)
        goto out;
    if (mbedtls_x509_crt_parse_file(&cert, g_cert) != 0)
        goto out;
    if (mbedtls_pk_parse_keyfile(&key, g_key, NULL, mbedtls_ctr_drbg_random, &drbg) != 0)
        goto out;
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto out;
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0)
        goto out;
    if (mbedtls_ssl_setup(&ssl, &conf) != 0)
        goto out;
    mbedtls_ssl_set_bio(&ssl, &client_fd, bio_send, bio_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto out;
    }
    s->served = 1;

    /* Read the request headers so the handshake and the send both really happen. */
    {
        char buf[2048];
        size_t got = 0;
        while (got < sizeof(buf) - 1)
        {
            ret = mbedtls_ssl_read(&ssl, (unsigned char *)buf + got, 1);
            if (ret <= 0)
                break;
            got += (size_t)ret;
            buf[got] = '\0';
            if (got >= 4 && strcmp(buf + got - 4, "\r\n\r\n") == 0)
                break;
        }
    }

    if (s->mode == RESP_SILENT)
    {
        /* Answer nothing: the probe must give up on its own timeout. Hold the connection
         * until the probe's client goes away, so the test measures the timeout and not a
         * server that vanished early. */
        char c;
        for (;;)
        {
            ret = mbedtls_ssl_read(&ssl, (unsigned char *)&c, 1);
            if (ret <= 0)
                break;
        }
    }
    else
    {
        const char *body = "{\"ok\":true}";
        char resp[512];
        const char *status = (s->mode == RESP_401) ? "401 Unauthorized" : "200 OK";
        const int n = snprintf(resp, sizeof(resp),
                               "HTTP/1.1 %s\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "%s",
                               status, strlen(body), body);
        if (mbedtls_ssl_write(&ssl, (const unsigned char *)resp, (size_t)n) > 0)
            s->served = 2;
    }

out:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cert);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
}

static int server_start(struct tls_server *s, enum resp_mode mode)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);

    memset(s, 0, sizeof(*s));
    s->mode = mode;
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* let the kernel pick, so parallel runs cannot collide */
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;
    if (listen(s->listen_fd, 4) != 0)
        return -1;
    if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen) != 0)
        return -1;
    s->port = ntohs(addr.sin_port);

    if (uv_thread_create(&s->thread, server_thread, s) != 0)
        return -1;
    return 0;
}

static void server_stop(struct tls_server *s)
{
    uv_thread_join(&s->thread);
    close(s->listen_fd);
}

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
        struct tls_server s;
        char out[256];
        char url[128];
        if (server_start(&s, RESP_200) != 0)
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
        server_stop(&s);
    }

    {
        struct tls_server s;
        char out[256];
        char url[128];
        if (server_start(&s, RESP_401) != 0)
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
        server_stop(&s);
    }
}

static void test_timeout(void)
{
    printf("\n── the endpoint says nothing ──\n");
    struct tls_server s;
    char out[256];
    char url[128];
    if (server_start(&s, RESP_SILENT) != 0)
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
    server_stop(&s);
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
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_endpoint_probe_%ld",
             (tmp && *tmp) ? tmp : "/tmp", (long)getpid());
    snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", g_dir);
    snprintf(g_key, sizeof(g_key), "%s/key.pem", g_dir);

    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "    (warning: could not create %s)\n", g_dir);

    printf("workspace: %s\n", g_dir);

    if (write_cert_and_key() != 0)
    {
        fprintf(stderr, "cannot generate the test certificate; skipping\n");
        return 1;
    }

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
