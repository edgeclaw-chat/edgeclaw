/* tests/test_proxy.c -- HTTPS through an HTTP CONNECT proxy.
 *
 * Two halves, because the feature has two halves:
 *
 *   - which proxy to use is a pure decision over strings (edc_proxy_resolve), and its cases are
 *     the ones that go wrong in the field: a value with no scheme, credentials, a no_proxy entry
 *     that should exclude the host, a socks proxy that must be refused rather than ignored;
 *   - speaking CONNECT is not, so it is driven against a local server that answers the
 *     preamble and then does TLS on the same socket -- the shape a tunnel actually presents.
 *
 * The second half is what catches a client that "supports proxies" by resolving the proxy and
 * then still sending the request to it directly: the CONNECT line has to name the TARGET, and
 * the TLS hostname has to be the target's, or the tunnel is useless.
 */

#include "edc_https_client.h"
#include "edc_proxy.h"

#include "sds.h"
#include "uv.h"

#include "tls_stub.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

/* Every proxy variable, cleared between cases: a case that forgets one would pass for the
 * wrong reason on a machine that has a proxy configured. */
static void clear_proxy_env(void)
{
    static const char *const vars[] = {"https_proxy", "HTTPS_PROXY", "http_proxy",  "HTTP_PROXY",
                                       "all_proxy",   "ALL_PROXY",   "no_proxy",    "NO_PROXY"};
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
        unsetenv(vars[i]);
}

static void test_resolution(void)
{
    printf("\n── which proxy applies ──\n");

    struct edc_proxy p;
    const char *why = NULL;

    clear_proxy_env();
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "nothing set: no proxy");
    CHECK(why == NULL, "and no complaint -- nothing was configured");

    setenv("https_proxy", "http://proxy.corp:3128", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "https_proxy with a scheme is used");
    CHECK(p.host && strcmp(p.host, "proxy.corp") == 0, "the host comes out");
    CHECK(p.port == 3128, "and the port");
    CHECK(p.auth == NULL, "with no credentials");
    edc_proxy_clear(&p);

    /* Everything after this point assumes the unset state. */
    clear_proxy_env();
    /* A bare host:port, which is what people paste from a wiki. */
    setenv("https_proxy", "proxy.corp:8080", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "a value with no scheme is accepted");
    CHECK(p.host && strcmp(p.host, "proxy.corp") == 0, "with the host");
    CHECK(p.port == 8080, "and the port");
    edc_proxy_clear(&p);

    clear_proxy_env();
    setenv("https_proxy", "proxy.corp", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "a bare host is accepted");
    CHECK(p.port == 80, "and defaults to port 80");
    edc_proxy_clear(&p);

    clear_proxy_env();
    setenv("HTTPS_PROXY", "http://PROXY.corp:3128/", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "the upper-case variable is read too");
    CHECK(p.host && strcmp(p.host, "PROXY.corp") == 0, "a trailing slash is not part of the host");
    edc_proxy_clear(&p);

    clear_proxy_env();
    setenv("https_proxy", "http://user:secret@proxy.corp:3128", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "credentials are parsed");
    CHECK(p.auth != NULL, "into an auth header");
    /* base64("user:secret") == dXNlcjpzZWNyZXQ= */
    CHECK(p.auth && strcmp(p.auth, "Basic dXNlcjpzZWNyZXQ=") == 0, "with the right base64 payload");
    CHECK(p.host && strcmp(p.host, "proxy.corp") == 0, "and the host is not confused by the '@'");
    edc_proxy_clear(&p);

    clear_proxy_env();
    setenv("all_proxy", "http://proxy.corp:3128", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "all_proxy is the fallback");
    edc_proxy_clear(&p);

    /* The remaining shapes are the ones a hand-typed value takes. */
    clear_proxy_env();
    setenv("https_proxy", "http://[::1]:3128", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "a bracketed IPv6 proxy is accepted");
    CHECK(p.host && strcmp(p.host, "::1") == 0, "with the brackets removed from the host");
    CHECK(p.port == 3128, "and the port after them");
    edc_proxy_clear(&p);

    clear_proxy_env();
    setenv("https_proxy", "http://[::1]", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "a bracketed IPv6 proxy with no port is accepted");
    CHECK(p.host && strcmp(p.host, "::1") == 0, "with the address");
    CHECK(p.port == 80, "and the default port");
    edc_proxy_clear(&p);

    clear_proxy_env();
    setenv("https_proxy", "http://[::1:3128", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "an unterminated bracket is refused");
    CHECK(why != NULL, "with a reason naming it");

    clear_proxy_env();
    setenv("https_proxy", "http://proxy.corp:70000", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "a port out of range is refused");
    CHECK(why != NULL, "with a reason");

    /* out == NULL asks "would a proxy be used", and must not leak the auth string. */
    clear_proxy_env();
    setenv("https_proxy", "http://user:pw@proxy.corp:3128", 1);
    CHECK(edc_proxy_resolve("api.github.com", NULL, &why), "the question can be asked without an output");
    clear_proxy_env();

    /* Clearing a cleared struct, and a NULL one, are no-ops rather than crashes. */
    struct edc_proxy empty = {0};
    edc_proxy_clear(&empty);
    edc_proxy_clear(NULL);
    CHECK(true, "clearing an empty or NULL proxy is a no-op");

    /* An empty userinfo ("http://@proxy") is not credentials. */
    clear_proxy_env();
    setenv("https_proxy", "http://@proxy.corp:3128", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "an empty userinfo is accepted");
    CHECK(p.auth == NULL, "and produces no auth header");
    CHECK(p.host && strcmp(p.host, "proxy.corp") == 0, "with the host still parsed");
    edc_proxy_clear(&p);

    /* A no_proxy entry may carry a port; the host is what matters. */
    clear_proxy_env();
    setenv("https_proxy", "http://proxy.corp:3128", 1);
    setenv("no_proxy", "api.github.com:443", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "a no_proxy entry with a port still excludes the host");

    /* Separators with nothing between them, and a trailing one. */
    setenv("no_proxy", " , other.example , ", 1);
    CHECK(edc_proxy_resolve("api.github.com", &p, &why), "empty entries are skipped, not treated as a match");
    edc_proxy_clear(&p);
    clear_proxy_env();
}

static void test_refusal_and_no_proxy(void)
{
    printf("\n── what is refused, and what is excluded ──\n");

    struct edc_proxy p;
    const char *why = NULL;

    clear_proxy_env();
    setenv("https_proxy", "socks5://proxy.corp:1080", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "a socks proxy is not used");
    CHECK(why != NULL, "and the caller is told why, rather than silently going direct");
    printf("      reason: %s\n", why);

    clear_proxy_env();
    setenv("https_proxy", "http://", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "a value with no host is refused");
    CHECK(why != NULL, "with a reason");

    /* no_proxy: the suffix rules that matter. */
    clear_proxy_env();
    setenv("https_proxy", "http://proxy.corp:3128", 1);
    setenv("no_proxy", "github.com", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "no_proxy excludes a subdomain");
    CHECK(edc_proxy_resolve("raw.githubusercontent.com", &p, &why),
          "and a different domain still goes through the proxy");
    edc_proxy_clear(&p);

    clear_proxy_env();
    setenv("https_proxy", "http://proxy.corp:3128", 1);
    setenv("no_proxy", "api.github.com", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "an exact host is excluded");

    setenv("no_proxy", ".github.com", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "a leading dot is accepted");

    setenv("no_proxy", "example.com, github.com", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "a comma-separated list is accepted");

    setenv("no_proxy", "*", 1);
    CHECK(!edc_proxy_resolve("anything.example", &p, &why), "\"*\" excludes everything");

    /* Case-insensitive, because the variable's value is a hostname. */
    setenv("no_proxy", "API.GitHub.com", 1);
    CHECK(!edc_proxy_resolve("api.github.com", &p, &why), "the comparison ignores case");

    clear_proxy_env();
}

/* The tunnel: the stub answers CONNECT, then runs TLS on the same socket. */
struct seen
{
    uv_sem_t done;
    int status;
    int headers;
    sds body;
    char error[256];
    bool closed;
    bool timed_out;
};

static void on_event(struct edc_https_request *req, int ev, void *data)
{
    struct seen *s = edc_https_request_get_priv(req);
    if (!s)
        return;
    switch (ev)
    {
    case EDC_HTTPS_EVENT_HS: {
        const char *line = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        edc_https_request_write(req, (const unsigned char *)line, strlen(line));
        break;
    }
    case EDC_HTTPS_EVENT_HEADER:
        s->headers++;
        break;
    case EDC_HTTPS_EVENT_MSG: {
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
        snprintf(s->error, sizeof(s->error), "%s", data ? (const char *)data : "");
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

static void test_connect_tunnel(const char *cert_path, const char *key_path)
{
    printf("\n── the request goes through the tunnel ──\n");

    char raw[256];
    tls_stub_response(raw, sizeof(raw), "200 OK", "{\"via\":\"proxy\"}");

    struct tls_stub proxy;
    if (tls_stub_start_proxy(&proxy, cert_path, key_path, raw) != 0)
    {
        CHECK(false, "could not start the proxy stub");
        return;
    }

    /* The URL names a port nothing listens on: if the client connected there directly, the case
     * would fail rather than quietly passing through the proxy by luck. */
    const int target_port = 9; /* discard */
    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d/v1/models", target_port);

    char proxy_value[64];
    snprintf(proxy_value, sizeof(proxy_value), "http://127.0.0.1:%d", proxy.port);
    setenv("https_proxy", proxy_value, 1);
    setenv("HTTPS_PROXY", proxy_value, 1);
    unsetenv("no_proxy");
    unsetenv("NO_PROXY");

    struct seen s;
    memset(&s, 0, sizeof(s));
    uv_sem_init(&s.done, 0);

    struct edc_https_client *client = edc_https_client_new(cert_path);
    CHECK(client != NULL, "the client was created");
    if (client)
    {
        const int rc = edc_https_client_connect(client, url, on_event, &s);
        CHECK(rc == 0, "connect() accepted the request");

        uv_thread_t dog;
        const bool dog_ok = uv_thread_create(&dog, watchdog, &s) == 0;
        uv_sem_wait(&s.done);
        if (dog_ok)
            uv_thread_join(&dog);

        CHECK(s.closed && !s.timed_out, "the request completed rather than hanging");
        CHECK(s.error[0] == '\0', "no error was reported");
        CHECK(s.status == 200, "the status came back through the tunnel");
        CHECK(s.body && strcmp(s.body, "{\"via\":\"proxy\"}") == 0, "and so did the body");

        printf("      CONNECT request: %.60s\n", tls_stub_last_connect);
        char expect[64];
        snprintf(expect, sizeof(expect), "CONNECT localhost:%d HTTP/1.1", target_port);
        CHECK(strncmp(tls_stub_last_connect, expect, strlen(expect)) == 0,
              "the CONNECT line names the TARGET host and port, not the proxy's");

        edc_https_client_free(client);
    }
    uv_sem_destroy(&s.done);
    if (s.body)
        sdsfree(s.body);
    tls_stub_stop(&proxy);
    clear_proxy_env();
}

int main(void)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/edc_test_proxy_%ld", (tmp && *tmp) ? tmp : "/tmp", (long)getpid());
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0)
        return 1;

    char cert[600];
    char key[600];
    snprintf(cert, sizeof(cert), "%s/cert.pem", dir);
    snprintf(key, sizeof(key), "%s/key.pem", dir);
    if (tls_stub_cert(cert, key) != 0)
    {
        fprintf(stderr, "cannot generate a certificate\n");
        return 1;
    }

    test_resolution();
    test_refusal_and_no_proxy();
    test_connect_tunnel(cert, key);

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", dir);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
