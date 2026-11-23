#include "fake_release.h"

#include "edc_update.h"
#include "sds.h"
#include "tls_stub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_fs_util.h"

static struct tls_stub g_stub;
static int g_started = 0;
static char g_dir[512];
static char g_cert[600];
static char g_key[600];
static sds g_response = NULL;
static sds g_response2 = NULL;

/* Shared setup: make a certificate, serve |raw| verbatim, report where to reach it.
 *
 * |raw| is the complete HTTP response, or NULL for "handshake, then say nothing" -- the
 * latter is what an unresponsive endpoint looks like, and a CLI case needs it to reach the
 * timeout branch. */
/* A HEAD response saying the probed archive exists. */
static sds head_ok_response(void)
{
    return sdsnew("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

/* |atom| != NULL means multi mode: serve the feed on the first connection and a 200 HEAD
 * answer (the archive probe) on the rest, and report the URL with a .atom suffix. */
static int start_serving(const char *raw, char *url_out, size_t url_cap, char *cert_out, size_t cert_cap,
                         bool atom)
{
    const char *tmp = getenv("TEST_TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        return -1;

    snprintf(g_dir, sizeof(g_dir), "%s/edc_fake_release_%ld", tmp, (long)getpid());
    snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", g_dir);
    snprintf(g_key, sizeof(g_key), "%s/key.pem", g_dir);
    test_rm_rf(g_dir);
    test_mkdir_p(g_dir);

    if (tls_stub_cert(g_cert, g_key) != 0)
        return -1;

    sdsfree(g_response);
    g_response = raw ? sdsnew(raw) : NULL;

    if (tls_stub_start(&g_stub, g_cert, g_key, g_response) != 0)
    {
        sdsfree(g_response);
        g_response = NULL;
        test_rm_rf(g_dir);
        return -1;
    }
    g_started = 1;

    if (atom)
    {
        sdsfree(g_response2);
        g_response2 = head_ok_response();
        g_stub.raw_response_2 = g_response2;
        g_stub.multi = true;
        g_stub.max_requests = 12;
        snprintf(url_out, url_cap, "https://localhost:%d/repos/o/r/releases.atom", g_stub.port);
    }
    else
    {
        snprintf(url_out, url_cap, "https://localhost:%d/repos/o/r/releases", g_stub.port);
    }
    snprintf(cert_out, cert_cap, "%s", g_cert);
    return 0;
}

/* A complete HTTP/1.1 response with a JSON body. Connection: close so the client reads to EOF
 * instead of waiting on a keep-alive it will never reuse. */
static sds http_response(int status, const char *reason, const char *body)
{
    return sdscatprintf(sdsempty(),
                        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n%s",
                        status, reason, strlen(body), body);
}

int fake_release_start(const char *json, char *url_out, size_t url_cap, char *cert_out, size_t cert_cap)
{
    if (!json || !url_out || url_cap == 0 || !cert_out || cert_cap == 0)
        return -1;

    sds raw = http_response(200, "OK", json);
    const int rc = start_serving(raw, url_out, url_cap, cert_out, cert_cap, false);
    sdsfree(raw);
    return rc;
}

int fake_release_start_status(int status, const char *reason, const char *body, char *url_out, size_t url_cap,
                              char *cert_out, size_t cert_cap)
{
    if (!url_out || url_cap == 0 || !cert_out || cert_cap == 0)
        return -1;

    sds raw = http_response(status, reason ? reason : "", body ? body : "");
    const int rc = start_serving(raw, url_out, url_cap, cert_out, cert_cap, false);
    sdsfree(raw);
    return rc;
}

int fake_release_start_silent(char *url_out, size_t url_cap, char *cert_out, size_t cert_cap)
{
    if (!url_out || url_cap == 0 || !cert_out || cert_cap == 0)
        return -1;
    return start_serving(NULL, url_out, url_cap, cert_out, cert_cap, false);
}

int fake_release_start_newer(const char *tag, char *url_out, size_t url_cap, char *cert_out, size_t cert_cap)
{
    /* The check now reads an Atom feed and HEAD-probes the archive. Serve a feed naming |tag|
     * on the first connection and a 200 HEAD answer on the probe; the URL comes back with a
     * .atom suffix so the client derives the matching /releases/download base. */
    sds feed = sdscatprintf(sdsempty(),
                            "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\">"
                            "<entry><id>tag:github.com,2008:Repository/123/%s</id></entry></feed>",
                            tag);
    sds raw = http_response(200, "OK", feed);
    const int rc = start_serving(raw, url_out, url_cap, cert_out, cert_cap, true);
    sdsfree(raw);
    sdsfree(feed);
    return rc;
}

void fake_release_stop(void)
{
    if (g_started)
    {
        tls_stub_stop(&g_stub);
        g_started = 0;
    }
    sdsfree(g_response);
    g_response = NULL;
    sdsfree(g_response2);
    g_response2 = NULL;
    if (g_dir[0])
    {
        test_rm_rf(g_dir);
        g_dir[0] = '\0';
    }
}
