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
    int served;               /* 1 = handshake, 2 = response fully written, 3 = partial write */
    /* End the connection with a RST instead of a TLS close_notify.
     *
     * The default is the polite shutdown: mbedtls_ssl_close_notify, so the client reads
     * PEER_CLOSE_NOTIFY and treats the socket close as the normal end of the response. That
     * is the right fixture for "the server finished answering", and it is why the client's
     * read-error and write-error branches had no coverage at all -- a polite peer never
     * produces them.
     *
     * With this set the server skips the notify and closes with SO_LINGER{1,0}, which sends a
     * RST: the client's next read fails with a socket error, and a write to it fails too.
     * That is what a crash, a killed process, or a NAT timeout looks like from the other
     * side, and it is a different client path from a clean close. */
    bool abrupt_close;

    /* After a completed handshake, write bytes straight to the socket instead of answering
     * through TLS: the client's mbedtls then reads a record it cannot decrypt. */
    bool garbage_after_handshake;
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
static int tls_stub_cert_named(const char *cert_path, const char *key_path, const char *common_name)
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

    {
        const char *cn = (common_name && common_name[0]) ? common_name : "localhost";
        char subject[128];
        snprintf(subject, sizeof(subject), "CN=%s,O=EdgeClaw", cn);
        if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0)
            goto out;
        if (mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0)
            goto out;
    }
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

/* The common case: a certificate for "localhost". */
static int tls_stub_cert(const char *cert_path, const char *key_path)
{
    return tls_stub_cert_named(cert_path, key_path, "localhost");
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

/* The request line and headers the last case sent, for tests that need to check what the
 * CLIENT asked for rather than what the server answered. Empty until a request arrives. */
static char tls_stub_last_request[4096];
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
     * case cannot pass by accident on a connection that never carried a request.
     *
     * What was read is KEPT in tls_stub_last_request, because the request line is otherwise
     * the one part of the exchange no test ever looked at. A malformed target ("GET repos/x"
     * without the leading slash) got a 400 from the server, and every case still passed --
     * they assert on what the server SENDS, never on what the client asked for. See
     * test_update_check's "the request line" case. */
    {
        size_t got = 0;
        while (got < sizeof(tls_stub_last_request) - 1)
        {
            ret = mbedtls_ssl_read(&ssl, (unsigned char *)tls_stub_last_request + got, 1);
            if (ret <= 0)
                break;
            got += (size_t)ret;
            tls_stub_last_request[got] = '\0';
            if (got >= 4 && strcmp(tls_stub_last_request + got - 4, "\r\n\r\n") == 0)
                break;
        }
    }

    if (s->garbage_after_handshake)
    {
        /* A syntactically shaped TLS record whose payload is not a valid ciphertext: the
         * client's mbedtls accepts the framing and fails the record authentication, which is
         * a read ERROR rather than a socket error. That distinction is the point -- a reset
         * surfaces in libuv's read callback, while this surfaces inside mbedtls, and the two
         * take different branches in the client. */
        static const unsigned char bad_record[] = {
            0x17, 0x03, 0x03, 0x00, 0x08, /* application_data, TLS 1.2, length 8 */
            'b',  'a',  'd',  'r',  'e',  'c',  'o',  'd',
        };
        size_t off = 0;
        while (off < sizeof(bad_record))
        {
            const ssize_t n = send(client_fd, bad_record + off, sizeof(bad_record) - off, 0);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
    }
    else if (!s->raw_response)
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
        /* Written in a loop, not in one call: a single mbedtls_ssl_write returns at most one
         * TLS record (16 KiB), so a larger response was silently truncated. That was not a
         * theoretical concern -- serving a real 37 KiB release list through this stub produced
         * a body that stopped mid-JSON, the parser rejected it, and the check reported "no
         * update available" exactly as it would for a genuine empty list. A fixture that
         * quietly loses data makes a broken client look correct.
         *
         * The loop also has to handle a short write from flow control, which is why it retries
         * rather than trusting the first return value. */
        const unsigned char *body = (const unsigned char *)s->raw_response;
        const size_t len = strlen(s->raw_response);
        size_t sent = 0;
        while (sent < len)
        {
            const int n = mbedtls_ssl_write(&ssl, body + sent, len - sent);
            if (n <= 0)
                break; /* MBEDTLS_ERR_SSL_WANT_WRITE cannot happen on a blocking socket */
            sent += (size_t)n;
        }
        if (sent == len)
            s->served = 2;
        else
            s->served = 3; /* partial: the test should fail loudly rather than see a short body */
    }

out:
    if (!s->abrupt_close)
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
        if (s->abrupt_close)
        {
            /* SO_LINGER{on=1, seconds=0}: close() discards the send queue and sends a RST
             * rather than a FIN. The peer's read then fails with ECONNRESET instead of seeing
             * an orderly end-of-stream, which is the only way to reach the client's
             * socket-error branches from a test. */
            struct linger lg;
            lg.l_onoff = 1;
            lg.l_linger = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        }
        close(client_fd);
    }
}

/* Listen on ::1 only. Same server, same certificate checks -- the difference is the family.
 *
 * Exists so a case can reproduce "the first address the resolver offers is unreachable": this
 * system maps `ip6-localhost` to ::1 and nothing else, so a client that asks only for IPv4
 * finds no address at all, while one that asks for both reaches it.
 *
 * Defined after tls_stub_thread (below) so it can start it; the forward declaration of that
 * thread is not needed because this header's functions are all static and ordered. */
static int tls_stub_start_v6(struct tls_stub *s, const char *cert_path, const char *key_path,
                             const char *raw_response);

/* Start listening on a kernel-assigned loopback port. Returns 0 on success, -1 on failure. */
static int tls_stub_start(struct tls_stub *s, const char *cert_path, const char *key_path,
                          const char *raw_response)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);

    memset(s, 0, sizeof(*s));
    s->raw_response = raw_response;
    /* Start each case from an empty capture, so a test cannot read the previous one's request. */
    tls_stub_last_request[0] = '\0';
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

static int tls_stub_start_v6(struct tls_stub *s, const char *cert_path, const char *key_path,
                             const char *raw_response)
{
    struct sockaddr_in6 addr;
    socklen_t alen = sizeof(addr);

    memset(s, 0, sizeof(*s));
    s->raw_response = raw_response;
    tls_stub_last_request[0] = '\0';
    s->listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (s->listen_fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = 0; /* kernel picks, as in the IPv4 stub */
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;
    if (listen(s->listen_fd, 4) != 0)
        return -1;
    if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen) != 0)
        return -1;
    s->port = ntohs(addr.sin6_port);

    tls_stub_cert_file = cert_path;
    tls_stub_key_file = key_path;
    if (uv_thread_create(&s->thread, tls_stub_thread, s) != 0)
        return -1;
    return 0;
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
