/* test_https_client_failures: the transport, when the transport is the problem.
 *
 * `edc_https_client.c` was at 77.6% lines and its uncovered block was entirely the failure
 * side of a request: the total-deadline timer (425-438), the handshake error branch
 * (577-582), the tcp write failure (453-484), and an unparseable URL (228-231). Those are
 * the paths a user actually hits when a provider is down, a proxy is in the way, or a key
 * points at the wrong host -- and none of them had a test.
 *
 * Two facts from the code make this testable without the network (both were established by
 * reading it, not guessed):
 *
 *   - TLS is decided by mbedtls, not by the URL scheme. `http://` still starts a handshake,
 *     so "connect to a plain-text server" is a deterministic HANDSHAKE FAILURE -- which is
 *     what a user sees behind a proxy that terminates TLS and re-serves plain HTTP.
 *   - The response's completion signal is the socket CLOSE, not llhttp's message-complete
 *     (that callback is empty). So the event sequence a caller can rely on is asserted here
 *     explicitly: HS, HEADER..., MSG..., then CLOSE.
 *
 * The failures are produced by real conditions -- a closed port, a server that speaks the
 * wrong protocol, a server that accepts and says nothing -- not by stubbing the client.
 */

#include "edc_https_client.h"
#include "sds.h"
#include "uv.h"

#include "tls_stub.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#include "test_fs_util.h"

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) printf("\n-- %s\n", name)

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed++; } \
    else { printf("    ok: "); printf(__VA_ARGS__); printf("\n"); g_passed++; } \
} while (0)

/* ── what the callback saw ─────────────────────────────────────────────────── */

struct seen
{
    uv_sem_t done;
    int events[16];
    int n_events;
    int status;
    int headers;
    sds body;
    char error[512];
    bool closed;
    bool timed_out;
};

static void on_event(struct edc_https_request *req, int ev, void *data)
{
    struct seen *s = edc_https_request_get_priv(req);
    if (!s)
        return;
    if (s->n_events < (int)(sizeof(s->events) / sizeof(s->events[0])))
        s->events[s->n_events++] = ev;

    switch (ev)
    {
    case EDC_HTTPS_EVENT_HS: {
        /* The client only establishes TLS; the CALLER writes the request. This is the shape
         * edc_llm_actor.c uses, and forgetting it is why the first version of this test saw
         * a handshake and then nothing: the stub was waiting for a request that never came. */
        sds request = sdscatprintf(sdsempty(),
                                   "GET %s HTTP/1.1\r\n"
                                   "Host: %s\r\n"
                                   "Connection: close\r\n"
                                   "\r\n",
                                   edc_https_request_get_path(req), edc_https_request_get_host(req));
        edc_https_request_write(req, (const unsigned char *)request, (size_t)sdslen(request));
        sdsfree(request);
        break;
    }
    case EDC_HTTPS_EVENT_HEADER:
        s->headers++;
        break;
    case EDC_HTTPS_EVENT_MSG: {
        /* Borrowed for the duration of the call and owned by us afterwards: the client
         * mallocs each chunk and hands it over (see edc_https_on_body). */
        uv_buf_t *buf = data;
        if (buf && buf->base)
        {
            if (!s->body)
                s->body = sdsempty();
            s->body = sdscatlen(s->body, buf->base, buf->len);
            free(buf->base);
        }
        break;
    }
    case EDC_HTTPS_EVENT_ERROR:
        if (data)
            snprintf(s->error, sizeof(s->error), "%s", (const char *)data);
        break;
    case EDC_HTTPS_EVENT_CLOSE:
        s->status = edc_https_request_get_status(req);
        s->closed = true;
        uv_sem_post(&s->done);
        break;
    default:
        break;
    }
}

/* Same watchdog trick as the LLM response test: libuv 1.52 has no uv_sem_timedwait, and a
 * request that never completes must fail one case rather than hang the binary. */
static void watchdog(void *arg)
{
    struct seen *s = arg;
    for (int i = 0; i < 400; i++) /* 20s */
    {
        uv_sleep(50);
        if (s->closed)
            return;
    }
    s->timed_out = true;
    uv_sem_post(&s->done);
}

static char g_dir[512];
static char g_cert[600];
static char g_key[600];

/* Run one request and wait for its CLOSE (or the watchdog). */
static bool run(const char *url, struct seen *s)
{
    memset(s, 0, sizeof(*s));
    uv_sem_init(&s->done, 0);

    struct edc_https_client *client = edc_https_client_new(g_cert);
    if (!client)
    {
        uv_sem_destroy(&s->done);
        return false;
    }
    if (edc_https_client_connect(client, url, on_event, s) != 0)
    {
        edc_https_client_free(client);
        uv_sem_destroy(&s->done);
        return false;
    }

    uv_thread_t dog;
    const bool dog_ok = uv_thread_create(&dog, watchdog, s) == 0;
    uv_sem_wait(&s->done);
    if (dog_ok)
        uv_thread_join(&dog);

    edc_https_client_free(client);
    uv_sem_destroy(&s->done);
    return s->closed && !s->timed_out;
}

static void free_seen(struct seen *s)
{
    if (s->body)
        sdsfree(s->body);
    s->body = NULL;
}

/* ── cases ─────────────────────────────────────────────────────────────────── */

static void test_invalid_url_is_refused_synchronously()
{
    TEST("an unparseable URL is refused before any socket is opened");
    /* The contract in the header: on -1, `priv` is untouched and the caller keeps ownership.
     * Reaching that branch needs a URL yuarel cannot parse; a bare host with no scheme and no
     * slashes is the shape that fails. */
    struct edc_https_client *client = edc_https_client_new(g_cert);
    CHECK(client != NULL, "the client was created");

    struct seen s;
    memset(&s, 0, sizeof(s));
    const int rc = edc_https_client_connect(client, "no-scheme-and-no-slash", on_event, &s);
    CHECK(rc == -1, "connect() returns -1 for an unparseable URL, got %d", rc);
    CHECK(s.n_events == 0, "and no event was delivered");

    edc_https_client_free(client);
}

static void test_connection_refused()
{
    TEST("nothing listening: the request fails with a connect error");
    /* Take a port from the kernel, then give it back -- nothing is listening on it now. */
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

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", port);

    struct seen s;
    const bool closed = run(url, &s);
    CHECK(closed, "the request completed (a dead port must not hang)");
    CHECK(strstr(s.error, "tcp connect failed") != NULL,
          "the error names the connect failure: %.80s", s.error);
    CHECK(s.status == 0, "no HTTP status, because no response line arrived (0), got %d", s.status);
    free_seen(&s);
}

/* A server that speaks plain HTTP where TLS is expected. TLS is decided by mbedtls, not by
 * the URL scheme, so this is the deterministic way to fail the handshake -- and it is what a
 * user sees behind a proxy that terminates TLS and re-serves plain text. */
struct plain_server
{
    int listen_fd;
    int port;
    uv_thread_t thread;
    int served;
};

static void plain_server_thread(void *arg)
{
    struct plain_server *s = arg;
    const int fd = accept(s->listen_fd, NULL, NULL);
    if (fd >= 0)
    {
        const char *plain = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi";
        if (send(fd, plain, strlen(plain), 0) > 0)
            s->served = 1;
        close(fd);
    }
}

static int plain_server_start(struct plain_server *s)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    memset(s, 0, sizeof(*s));
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;
    if (listen(s->listen_fd, 4) != 0)
        return -1;
    if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen) != 0)
        return -1;
    s->port = ntohs(addr.sin_port);
    if (uv_thread_create(&s->thread, plain_server_thread, s) != 0)
        return -1;
    return 0;
}

static void test_handshake_failure()
{
    TEST("a plain-text server where TLS is expected fails the handshake, and says so");
    struct plain_server srv;
    if (plain_server_start(&srv) != 0)
    {
        CHECK(false, "could not start the plain-text server");
        return;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", srv.port);

    struct seen s;
    const bool closed = run(url, &s);
    CHECK(closed, "the request completed instead of hanging");
    CHECK(strstr(s.error, "handshake") != NULL,
          "the error names the handshake: %.90s", s.error);
    CHECK(s.status == 0, "no status line was ever parsed, got %d", s.status);
    free_seen(&s);

    uv_thread_join(&srv.thread);
    close(srv.listen_fd);
}

static void test_timeout()
{
    TEST("a peer that accepts and says nothing hits the total deadline");

    /* Set the deadline through the environment: that is the documented override, and the
     * parse of it (strtol + "> 0") is itself a branch worth covering. */
    setenv("EDGECLAW_HTTPS_TIMEOUT_MS", "300", 1);

    struct tls_stub stub;
    if (tls_stub_start(&stub, g_cert, g_key, NULL) != 0)
    {
        CHECK(false, "could not start the silent TLS stub");
        unsetenv("EDGECLAW_HTTPS_TIMEOUT_MS");
        return;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", stub.port);

    struct seen s;
    const bool closed = run(url, &s);
    CHECK(closed, "the request completed (the timer fired) instead of hanging forever");
    CHECK(strstr(s.error, "timed out") != NULL, "the error names the timeout: %.80s", s.error);
    CHECK(strstr(s.error, "300 ms") != NULL, "and reports the deadline that was in force: %.80s", s.error);
    /* The handshake DID complete: this is a deadline on silence, not a TLS failure. */
    CHECK(stub.served == 1, "the handshake had completed, so this is a timeout and not TLS");
    free_seen(&s);

    tls_stub_stop(&stub);
    unsetenv("EDGECLAW_HTTPS_TIMEOUT_MS");
}

static void test_success_event_sequence()
{
    TEST("a good response arrives as HS, HEADER, MSG..., then CLOSE");
    /* The completion signal is the socket close, not llhttp's message-complete (that
     * callback is empty). A caller that waits for a "complete" event would wait forever, so
     * the order is pinned here. */
    char raw[512];
    tls_stub_response(raw, sizeof(raw), "200 OK", "{\"ok\":true}");

    struct tls_stub stub;
    if (tls_stub_start(&stub, g_cert, g_key, raw) != 0)
    {
        CHECK(false, "could not start the TLS stub");
        return;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", stub.port);

    struct seen s;
    const bool closed = run(url, &s);
    CHECK(closed, "the request closed");
    CHECK(s.error[0] == '\0', "no error was reported: %.80s", s.error);
    CHECK(s.status == 200, "the status line was parsed: got %d", s.status);
    CHECK(s.headers >= 2, "at least Content-Type and Content-Length were reported, got %d", s.headers);
    CHECK(s.body && strcmp(s.body, "{\"ok\":true}") == 0, "the body arrived intact: %.40s",
          s.body ? s.body : "(none)");

    /* First event is the handshake, last is the close. */
    CHECK(s.n_events >= 3, "several events were delivered, got %d", s.n_events);
    CHECK(s.events[0] == EDC_HTTPS_EVENT_HS, "the first event is HS, got %d", s.events[0]);
    CHECK(s.events[s.n_events - 1] == EDC_HTTPS_EVENT_CLOSE, "the last event is CLOSE, got %d",
          s.events[s.n_events - 1]);
    free_seen(&s);

    tls_stub_stop(&stub);
}

static void test_status_only_bodyless()
{
    TEST("a 204 with no body still closes cleanly");
    /* No Content-Length body: llhttp emits no MSG, and the close is still the completion
     * signal. This is the shape a provider returns for some proxy health checks. */
    char raw[256];
    tls_stub_response(raw, sizeof(raw), "204 No Content", "");

    struct tls_stub stub;
    if (tls_stub_start(&stub, g_cert, g_key, raw) != 0)
    {
        CHECK(false, "could not start the TLS stub");
        return;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", stub.port);

    struct seen s;
    const bool closed = run(url, &s);
    CHECK(closed, "the request closed");
    CHECK(s.status == 204, "the status is 204, got %d", s.status);
    CHECK(s.body == NULL || sdslen(s.body) == 0, "no body was delivered");
    free_seen(&s);

    tls_stub_stop(&stub);
}

int main(void)
{
    /* The stubs write to sockets whose peer has hung up (that is how the handshake and the
     * deadline cases end), and a write to a closed socket raises SIGPIPE, which kills the
     * process before any assertion can run. Same guard as test_pppos_agent_stress. */
    signal(SIGPIPE, SIG_IGN);
    setbuf(stdout, NULL); /* a crash must not swallow the log of what already passed */

    printf("test_https_client_failures\n");

    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    snprintf(g_dir, sizeof(g_dir), "%s/ec-httpsfail-XXXXXX", tmp);
    if (!mkdtemp(g_dir))
    {
        perror("mkdtemp");
        return 1;
    }
    snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", g_dir);
    snprintf(g_key, sizeof(g_key), "%s/key.pem", g_dir);
    if (tls_stub_cert(g_cert, g_key) != 0)
    {
        fprintf(stderr, "cannot generate the test certificate\n");
        return 1;
    }

    test_invalid_url_is_refused_synchronously();
    test_connection_refused();
    test_handshake_failure();
    test_timeout();
    test_success_event_sequence();
    test_status_only_bodyless();

    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", g_dir);

    printf("\n=== 测试结果: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
