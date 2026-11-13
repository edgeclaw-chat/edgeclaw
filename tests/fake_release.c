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

/* Shared setup: make a certificate, serve |raw| verbatim, report where to reach it.
 *
 * |raw| is the complete HTTP response, or NULL for "handshake, then say nothing" -- the
 * latter is what an unresponsive endpoint looks like, and a CLI case needs it to reach the
 * timeout branch. */
static int start_serving(const char *raw, char *url_out, size_t url_cap, char *cert_out, size_t cert_cap)
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

    snprintf(url_out, url_cap, "https://localhost:%d/repos/o/r/releases", g_stub.port);
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
    const int rc = start_serving(raw, url_out, url_cap, cert_out, cert_cap);
    sdsfree(raw);
    return rc;
}

int fake_release_start_status(int status, const char *reason, const char *body, char *url_out, size_t url_cap,
                              char *cert_out, size_t cert_cap)
{
    if (!url_out || url_cap == 0 || !cert_out || cert_cap == 0)
        return -1;

    sds raw = http_response(status, reason ? reason : "", body ? body : "");
    const int rc = start_serving(raw, url_out, url_cap, cert_out, cert_cap);
    sdsfree(raw);
    return rc;
}

int fake_release_start_silent(char *url_out, size_t url_cap, char *cert_out, size_t cert_cap)
{
    if (!url_out || url_cap == 0 || !cert_out || cert_cap == 0)
        return -1;
    return start_serving(NULL, url_out, url_cap, cert_out, cert_cap);
}

int fake_release_start_newer(const char *tag, char *url_out, size_t url_cap, char *cert_out, size_t cert_cap)
{
    sds json = sdscatprintf(sdsempty(),
                            "[{\"tag_name\":\"%s\",\"draft\":false,\"prerelease\":false,\"assets\":"
                            "[{\"name\":\"%s_%s.tar.gz\"}]}]",
                            tag, edc_update_target(), tag);
    const int rc = fake_release_start(json, url_out, url_cap, cert_out, cert_cap);
    sdsfree(json);
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
    if (g_dir[0])
    {
        test_rm_rf(g_dir);
        g_dir[0] = '\0';
    }
}
