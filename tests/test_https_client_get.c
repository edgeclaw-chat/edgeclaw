#include "log.h"
#include "edc_https_client.h"
#include "uv.h"
#include <stdio.h>
#include <stdlib.h>

/* Live test (one TCP/TLS round trip to example.com per case). It asserts what the
 * client now reports about the *response*: the HTTP status code, captured from
 * llhttp's status line and exposed via edc_https_request_get_status().
 *
 * Two cases on purpose: 200 alone would pass even if the value were hardcoded,
 * so the second case (a path example.com answers with 404) proves the number
 * actually follows the response. */

static uv_sem_t done_sem;
static int last_status;
static int last_headers_status; /* whatever the status is before the body */
static size_t body_len;

static void edc_https_client_fn(struct edc_https_request *req, int ev, void *data)
{
    if (ev == EDC_HTTPS_EVENT_HS)
    {
        static char body[256];
        int n = snprintf(body, sizeof(body),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         edc_https_request_get_path(req), edc_https_request_get_host(req));

        edc_https_request_write(req, (unsigned char *)body, n);
    }
    else if (ev == EDC_HTTPS_EVENT_HEADER)
    {
        struct edc_https_header *header = data;
        log_info("%s: %s", header->field, header->value);
        /* The status must already be there while headers stream in. */
        last_headers_status = edc_https_request_get_status(req);
    }
    else if (ev == EDC_HTTPS_EVENT_MSG)
    {
        uv_buf_t *buf = data;
        log_info("%.*s", (int)buf->len, buf->base);
        body_len += (size_t)buf->len;
        free(buf->base);
    }
    else if (ev == EDC_HTTPS_EVENT_ERROR)
    {
        log_info("edc_https_client: error event: %s", (const char *)data);
    }
    else if (ev == EDC_HTTPS_EVENT_CLOSE)
    {
        log_info("edc_https_client: connection closed");
        last_status = edc_https_request_get_status(req);
        uv_sem_post(&done_sem);
    }
}

/* 不给参数时用它 —— 否则测试在 bazel 下只会打 usage 就退出。 */
#define DEFAULT_URL "https://example.com/"
#define MISSING_PATH_URL "https://example.com/no-such-path-edgeclaw"

static struct edc_https_client *g_client;

static int fetch(const char *url)
{
    last_status = 0;
    last_headers_status = 0;
    body_len = 0;

    if (edc_https_client_connect(g_client, url, edc_https_client_fn, NULL) < 0)
    {
        fprintf(stderr, "invalid url: %s\n", url);
        return -1;
    }
    uv_sem_wait(&done_sem);
    return last_status;
}

int main(const int argc, const char *argv[])
{
    const char *url = (argc >= 2) ? argv[1] : DEFAULT_URL;
    int expect = (argc >= 3) ? atoi(argv[2]) : 200;
    int failures = 0;

    log_info("test_https_client_get: url=%s (期望 %d)", url, expect);

    uv_sem_init(&done_sem, 0);
    g_client = edc_https_client_new(NULL);

    int got = fetch(url);
    if (got != expect)
    {
        fprintf(stderr, "FAIL: %s ⇒ 状态码 %d, 期望 %d\n", url, got, expect);
        failures++;
    }
    else
    {
        fprintf(stderr, "ok: %s ⇒ %d (body %zu 字节, header 阶段读到 %d)\n", url, got, body_len, last_headers_status);
    }
    if (last_headers_status != got)
    {
        fprintf(stderr, "FAIL: header 阶段状态 %d ≠ CLOSE 时 %d（状态行应在头之前解析出来）\n", last_headers_status, got);
        failures++;
    }

    if (argc < 2)
    {
        /* 非 200：证明状态码跟着响应走，而不是恒 200。 */
        int missing = fetch(MISSING_PATH_URL);
        if (missing < 400 || missing > 599)
        {
            fprintf(stderr, "FAIL: %s ⇒ 状态码 %d, 期望 4xx/5xx\n", MISSING_PATH_URL, missing);
            failures++;
        }
        else
        {
            fprintf(stderr, "ok: %s ⇒ %d（非 200 路径）\n", MISSING_PATH_URL, missing);
        }
    }

    edc_https_client_free(g_client);
    uv_sem_destroy(&done_sem);
    fprintf(stderr, failures ? "=== %d 处失败 ===\n" : "=== 全部通过 ===\n", failures);
    return failures ? 1 : 0;
}
