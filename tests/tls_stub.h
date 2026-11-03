#ifndef EDC_TEST_TLS_STUB_H
#define EDC_TEST_TLS_STUB_H

/* ── A one-connection TLS server for tests, with no network and no checked-in key ──────
 *
 * Every TLS client in this tree insists on a real handshake: `edc_https_client` calls
 * mbedtls_ssl_setup unconditionally, and the LLM actor reads its trust anchor from
 * `<workspace>/ssl/cacert.pem`. So a test that wants to see a response -- a 429 body, a
 * truncated JSON, a `finish_reason` -- has to speak TLS to something. This is that
 * something: loopback only, kernel-assigned port, certificate generated per run (a committed
 * private key is still a private key, however scoped).
 *
 * Why a header rather than a shared .c: it keeps each test binary a single translation unit,
 * the same reason tests/test_ws.h is a header. Every function is `static`.
 *
 * Usage:
 *
 *     struct tls_stub s;
 *     tls_stub_cert(cert_path, key_path);                    // once per process
 *     tls_stub_start(&s, cert_path, key_path, raw_response); // raw_response may be NULL
 *     ... drive the client at https://localhost:<s.port>/...
 *     tls_stub_stop(&s);
 *
 * `raw_response` is the exact bytes to write after the request headers arrive, so a case can
 * be a status line plus any body at all. NULL means "accept the handshake and then say
 * nothing", which is how a client timeout is exercised without a flaky sleep.
 *
 * One connection per start: a case that makes two requests needs two stubs, or a loop.
 */

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
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct tls_stub
{
    int listen_fd;
    int port;
    uv_thread_t thread;
    const char *raw_response; /* borrowed; NULL = handshake only, then hold and stay silent */
    int served;               /* 1 = handshake completed, 2 = response written */
};

/* Generate a self-signed CA-ish certificate (CN=localhost) and its key at the given paths.
 *
 * CN and not a subjectAltName: mbedtls falls back to the CN when a certificate carries no
 * SAN (x509_crt.c, x509_crt_verify_name), so "localhost" in the client's URL matches without
 * building an extension. CA:TRUE + keyCertSign because a client that parses this file as its
 * trust anchor requires the signer to be allowed to sign.
 *
 * Returns 0 on success, -1 on failure.
 */
static int tls_stub_cert(const char *cert_path, const char *key_path)
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
        FILE *f = fopen(cert_path, "wb");
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
        FILE *f = fopen(key_path, "wb");
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

/* ── the server ─────────────────────────────────────────────────────────────── */

static int tls_stub_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    const int fd = *(int *)ctx;
    const ssize_t n = send(fd, buf, len, 0);
    if (n < 0)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)n;
}

static int tls_stub_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    const int fd = *(int *)ctx;
    const ssize_t n = recv(fd, buf, len, 0);
    if (n < 0)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (n == 0)
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return (int)n;
}

/* The certificate paths are handed over through file-scope strings rather than through the
 * thread argument: the worker function has one signature, and only one stub runs at a time
 * in a given test binary. */
static const char *tls_stub_cert_file;
static const char *tls_stub_key_file;

static void tls_stub_serve(struct tls_stub *s, int client_fd)
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
    if (mbedtls_x509_crt_parse_file(&cert, tls_stub_cert_file) != 0)
        goto out;
    if (mbedtls_pk_parse_keyfile(&key, tls_stub_key_file, NULL, mbedtls_ctr_drbg_random, &drbg) != 0)
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
    mbedtls_ssl_set_bio(&ssl, &client_fd, tls_stub_bio_send, tls_stub_bio_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto out;
    }
    s->served = 1;

    /* Read the request headers: the handshake and the client's send both really happen, so a
     * case cannot pass by accident on a connection that never carried a request. */
    {
        char buf[4096];
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

    if (!s->raw_response)
    {
        /* Silent mode: answer nothing and hold the connection until the client gives up, so
         * the case measures the client's own timeout rather than a server that vanished. */
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
        const size_t len = strlen(s->raw_response);
        if (mbedtls_ssl_write(&ssl, (const unsigned char *)s->raw_response, len) > 0)
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

static void tls_stub_thread(void *arg)
{
    struct tls_stub *s = arg;
    const int client_fd = accept(s->listen_fd, NULL, NULL);
    if (client_fd >= 0)
    {
        tls_stub_serve(s, client_fd);
        close(client_fd);
    }
}

/* Start listening on a kernel-assigned loopback port. Returns 0 on success, -1 on failure. */
static int tls_stub_start(struct tls_stub *s, const char *cert_path, const char *key_path,
                          const char *raw_response)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);

    memset(s, 0, sizeof(*s));
    s->raw_response = raw_response;
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

    tls_stub_cert_file = cert_path;
    tls_stub_key_file = key_path;
    if (uv_thread_create(&s->thread, tls_stub_thread, s) != 0)
        return -1;
    return 0;
}

static void tls_stub_stop(struct tls_stub *s)
{
    uv_thread_join(&s->thread);
    close(s->listen_fd);
}

/* Build a complete HTTP/1.1 response into |out|. `status_line` is e.g. "429 Too Many
 * Requests"; `body` may be empty but not NULL. Returns the byte count.
 *
 * `Connection: close` on purpose: the stub serves one connection, and a client that waits for
 * more data on a keep-alive connection would hang until its timeout. */
static int tls_stub_response(char *out, size_t cap, const char *status_line, const char *body)
{
    return snprintf(out, cap,
                    "HTTP/1.1 %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%s",
                    status_line, strlen(body), body);
}

#endif /* EDC_TEST_TLS_STUB_H */
