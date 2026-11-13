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
#include <fcntl.h>
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

/* A listener that accepts nothing, with its accept queue deliberately full.
 *
 * Why: the crash this guards against needs a connect that is still PENDING when the client is
 * freed. With a full backlog the kernel drops the SYN (tcp_abort_on_overflow=0), so the
 * client's connect never completes and libuv keeps it in flight -- deterministically, on
 * loopback, with no network involved. A blackhole address would depend on the sandbox's
 * routing table; this does not.
 */
struct full_backlog
{
    int listen_fd;
    int port;
    int fillers[4];
    int n_fillers;
};

static int full_backlog_start(struct full_backlog *b)
{
    memset(b, 0, sizeof(*b));
    b->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (b->listen_fd < 0)
        return -1;

    int on = 1;
    setsockopt(b->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(b->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;
    /* backlog 1: the queue holds one completed handshake, so a few non-blocking connects are
     * enough to make every later SYN be dropped. */
    if (listen(b->listen_fd, 1) != 0)
        return -1;
    if (getsockname(b->listen_fd, (struct sockaddr *)&addr, &alen) != 0)
        return -1;
    b->port = ntohs(addr.sin_port);

    for (int i = 0; i < 4; i++)
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            break;
        /* Non-blocking: the point is to occupy the queue, not to wait for an accept that
         * never comes. */
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        b->fillers[b->n_fillers++] = fd;
    }
    uv_sleep(200); /* let the queue fill */
    return 0;
}

static void full_backlog_stop(struct full_backlog *b)
{
    for (int i = 0; i < b->n_fillers; i++)
        if (b->fillers[i] >= 0)
            close(b->fillers[i]);
    if (b->listen_fd >= 0)
        close(b->listen_fd);
}

/* Freeing the client with a connect still in flight.
 *
 * This is the offline-launch crash, and it needs no network to reproduce:
 *
 *   - the launch-time update check has a 10s deadline;
 *   - offline, the connect is still pending when that deadline expires;
 *   - edc_update_check_ex then calls edc_https_client_free(), which uv_walk()s the loop and
 *     closes the socket;
 *   - libuv reports the cancellation to the pending connect callback as UV_ECANCELED (from
 *     uv__stream_destroy -> uv__finish_close);
 *   - that callback called uv_close() again, and uv_close asserts !uv__is_closing(handle).
 *
 * So the program aborts, in libuv, on an idle offline start -- which is what the user saw.
 * The assertion here is simply that free() returns.
 */
static void test_free_while_connect_in_flight()
{
    TEST("freeing the client with a connect in flight does not abort");

    struct full_backlog b;
    if (full_backlog_start(&b) != 0)
    {
        CHECK(false, "could not set up a hanging peer");
        full_backlog_stop(&b);
        return;
    }

    char url[128];
    /* A literal address, so getaddrinfo cannot be what holds the request up: the pending
     * thing must be the TCP connect, not a lookup. */
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/chat/completions", b.port);

    struct seen s;
    memset(&s, 0, sizeof(s));
    uv_sem_init(&s.done, 0);

    struct edc_https_client *client = edc_https_client_new(g_cert);
    CHECK(client != NULL, "the client was created");
    if (client)
    {
        const int rc = edc_https_client_connect(client, url, on_event, &s);
        CHECK(rc == 0, "connect() accepted the request (the connect itself stays pending), got %d", rc);

        /* Long enough for the resolver and uv_tcp_connect to have run, far short of any
         * timeout: the connect is in flight when free() is called. */
        uv_sleep(300);

        /* The vacuity guard, and the most important assertion in this test.
         *
         * If the connect had COMPLETED, free() would exercise the ordinary teardown and the
         * test would pass while proving nothing -- a green result for a case that never
         * happened. Silence is what says the request is still in flight: no HS (so no
         * completed TCP connect), no ERROR, no CLOSE. Every event this client can deliver
         * means it got past the state the crash needs.
         *
         * It also matters on a machine where the backlog cannot be filled (a different
         * net.core.somaxconn, or a kernel that resets instead of dropping): there the peer
         * answers, the events arrive, and this line reports the case as unusable rather than
         * as a pass. */
        CHECK(s.n_events == 0, "the request had not progressed yet (%d events) -- the connect was pending, "
                               "which is the state the crash needs",
              s.n_events);

        edc_https_client_free(client);
        /* Reaching this line is the assertion. Before the fix the process died inside
         * uv_close() above (glibc "Assertion `!uv__is_closing(handle)' failed"). */
        CHECK(true, "free() returned instead of aborting in uv_close");
    }

    uv_sem_destroy(&s.done);
    full_backlog_stop(&b);
}

/* Free a client at several points along the same request's life.
 *
 * The crash lives in one instant -- a handle closed by teardown while its connect callback is
 * still outstanding -- but the teardown code has several stages that can each decide to close
 * that handle: before the connect is issued, while it is pending, after it completes but
 * before the handshake, and during the handshake. A guard fixed at one of them and forgotten
 * at the next is exactly how this bug shipped (the write path had the guard, the connect path
 * did not), so the sweep is the point: it does not need to know which stage is broken.
 *
 * The peer accepts nothing and says nothing, so every delay leaves the request short of a
 * response. 0 covers "free before uv_tcp_connect ran"; the rest walk the window while the
 * connect is pending.
 */
static void test_free_at_every_stage()
{
    TEST("freeing the client at any stage of a request does not abort");

    struct full_backlog b;
    if (full_backlog_start(&b) != 0)
    {
        CHECK(false, "could not set up a hanging peer");
        full_backlog_stop(&b);
        return;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/chat/completions", b.port);

    static const int delays_ms[] = {0, 1, 10, 50, 250};
    for (size_t i = 0; i < sizeof(delays_ms) / sizeof(delays_ms[0]); i++)
    {
        struct seen s;
        memset(&s, 0, sizeof(s));
        uv_sem_init(&s.done, 0);

        struct edc_https_client *client = edc_https_client_new(g_cert);
        if (!client)
        {
            CHECK(false, "the client was created at delay %d ms", delays_ms[i]);
            uv_sem_destroy(&s.done);
            continue;
        }

        if (edc_https_client_connect(client, url, on_event, &s) != 0)
        {
            CHECK(false, "connect() accepted the request at delay %d ms", delays_ms[i]);
            edc_https_client_free(client);
            uv_sem_destroy(&s.done);
            continue;
        }

        if (delays_ms[i] > 0)
            uv_sleep((unsigned)delays_ms[i]);

        edc_https_client_free(client);
        /* Surviving the free is the assertion, at every one of these instants. */
        CHECK(s.n_events == 0, "delay %d ms: the request never completed, so free() tore down a live handle",
              delays_ms[i]);
        uv_sem_destroy(&s.done);
    }

    full_backlog_stop(&b);
}

/* The resolver fails: a hostname that cannot exist.
 *
 * `.invalid` is reserved by RFC 2606 precisely so it never resolves, which makes this a
 * deterministic DNS failure rather than one that depends on the sandbox having (or not
 * having) a working resolver or network. It reaches edc_https_on_resolved() with status < 0,
 * a path nothing else in this suite touches -- the other cases use a literal address
 * specifically so that name resolution cannot be what fails.
 */
static void test_resolution_failure()
{
    TEST("a host that cannot resolve fails as a connect error, not a hang");

    struct seen s;
    memset(&s, 0, sizeof(s));
    uv_sem_init(&s.done, 0);

    struct edc_https_client *client = edc_https_client_new(g_cert);
    CHECK(client != NULL, "the client was created");
    if (client)
    {
        const int rc = edc_https_client_connect(client, "https://edgeclaw.invalid/chat/completions", on_event, &s);
        CHECK(rc == 0, "connect() accepted the request, got %d", rc);

        uv_thread_t dog;
        const bool dog_ok = uv_thread_create(&dog, watchdog, &s) == 0;
        uv_sem_wait(&s.done);
        if (dog_ok)
            uv_thread_join(&dog);

        CHECK(s.closed && !s.timed_out, "the request reported a result instead of hanging");
        CHECK(strstr(s.error, "tcp connect failed") != NULL, "the error names the failure: %.90s", s.error);
        CHECK(s.status == 0, "no status line, because no connection was made, got %d", s.status);

        edc_https_client_free(client);
    }
    uv_sem_destroy(&s.done);
}

/* The peer dies without a TLS close_notify.
 *
 * A polite server ends the stream with close_notify, and the client treats the socket close
 * as the normal end of the response. A crash, a killed process or a NAT timeout looks
 * different: the connection is reset, and the client's read has to report an error rather
 * than a clean EOF. The stub's abrupt_close does that with SO_LINGER{1,0}.
 *
 * The assertion is that the caller still gets an error and still gets CLOSE -- not that any
 * particular errno appears, since which error mbedtls reports depends on where in the record
 * layer the reset lands.
 */
static void test_peer_resets_without_close_notify()
{
    TEST("a peer that resets mid-response is an error, not a clean end");

    /* A response that PROMISES more than it delivers, then a reset: this is a server that
     * died part-way through writing, which is the case the client has to distinguish from
     * "the response ended". Content-Length says 100, the body carries 8. */
    const char *partial = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: 100\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "{\"ok\":tr";

    struct tls_stub stub;
    if (tls_stub_start(&stub, g_cert, g_key, partial) != 0)
    {
        CHECK(false, "could not start the TLS stub");
        return;
    }
    /* Set before the client connects, so the stub's teardown sees it. */
    stub.abrupt_close = true;

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", stub.port);

    struct seen s;
    const bool finished = run(url, &s);
    CHECK(finished, "the request completed rather than hanging");
    CHECK(s.status == 200, "the status line did arrive, got %d", s.status);
    CHECK(strstr(s.error, "read failed") != NULL,
          "the truncation is reported as a read failure: %.90s", s.error);
    free_seen(&s);

    tls_stub_stop(&stub);
}

/* A peer that finishes the handshake and then speaks nonsense.
 *
 * This is the one path a socket error cannot reach: a reset is delivered by libuv to the read
 * callback (nread < 0), while a well-framed record that fails authentication is returned by
 * mbedtls_ssl_read itself. Different branches, different reporting, and the second had no
 * coverage at all because every fixture answered either in valid TLS or not at all.
 *
 * What a user sees is a proxy or middlebox that completed the handshake and then started
 * injecting its own bytes -- rare, but the failure mode is a hang if the error is not
 * reported, so it is worth a case.
 */
static void test_garbage_after_handshake()
{
    TEST("a peer that speaks non-TLS after the handshake is a read error");

    struct tls_stub stub;
    if (tls_stub_start(&stub, g_cert, g_key, "ignored") != 0)
    {
        CHECK(false, "could not start the TLS stub");
        return;
    }
    stub.garbage_after_handshake = true;

    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/chat/completions", stub.port);

    struct seen s;
    const bool finished = run(url, &s);
    CHECK(finished, "the request completed rather than hanging");
    CHECK(s.status == 0, "no response line could be parsed, got %d", s.status);
    CHECK(strstr(s.error, "read failed") != NULL, "reported as a read failure: %.90s", s.error);
    CHECK(strstr(s.error, "mbedtls") != NULL, "and names mbedtls, not the socket: %.90s", s.error);
    free_seen(&s);

    tls_stub_stop(&stub);
}

static void test_free_while_lookup_in_flight()
{
    TEST("freeing the client while the name lookup is in flight does not abort");

    struct edc_https_client *client = edc_https_client_new(g_cert);
    CHECK(client != NULL, "the client was created");
    if (client)
    {
        struct seen s;
        memset(&s, 0, sizeof(s));
        uv_sem_init(&s.done, 0);

        const int rc = edc_https_client_connect(client, "https://edgeclaw.invalid/chat/completions",
                                                on_event, &s);
        CHECK(rc == 0, "connect() accepted the request, got %d", rc);

        /* No sleep: the lookup has been queued and almost certainly not finished, which is
         * the state this case needs. */
        edc_https_client_free(client);
        /* Reaching this line is the assertion.
         *
         * This case LEAKS by design: uv_loop_close() refuses (the lookup is an active
         * request, not a handle), and the client is kept rather than freed under a loop that
         * still points into it. Every byte LeakSanitizer reports for this binary traces back
         * to this line, which is why it is spelled out here -- and why the project runs
         * --config=asan with detect_leaks=0 (.bazelrc: "several tests fork or hold a store
         * open on purpose"). */
        CHECK(true, "free() returned instead of aborting or corrupting the heap");
        uv_sem_destroy(&s.done);
    }
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
    test_resolution_failure();
    test_connection_refused();
    test_peer_resets_without_close_notify();
    test_garbage_after_handshake();
    test_free_while_connect_in_flight();
    test_free_while_lookup_in_flight();
    test_free_at_every_stage();
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
