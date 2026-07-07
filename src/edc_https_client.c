#include "edc_https_client.h"
#include "llhttp.h"
#include "log.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "sds.h"
#include "uv.h"
#include "yuarel.h"
#include <stdlib.h>
#include <string.h>

#define EDC_CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

struct edc_https_client
{
    uv_loop_t loop;
    uv_thread_t thread;
    uv_async_t work_async;
    uv_async_t stop_async;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config ssl_conf;
    mbedtls_x509_crt cacert;
};

struct edc_work
{
    char *url;
    edc_https_event_fn fn;
    void *priv;
};

struct edc_https_header
{
    const char *field;
    const char *value;
    struct edc_https_header *next;
};

struct edc_https_request
{
    uv_tcp_t socket;
    uv_connect_t conn;
    mbedtls_ssl_context ssl;

    llhttp_t parser;
    llhttp_settings_t parser_settings;

    struct yuarel url_parser;
    sds url;

    struct edc_https_client *client;
    edc_https_event_fn fn;

    struct edc_https_header header;

    void *priv;
};

static int edc_https_on_body(llhttp_t *http, const char *at, size_t length)
{
    return 0;
}
static int edc_https_on_header_field(llhttp_t *http, const char *at, size_t len)
{
    return 0;
}
static int edc_https_on_header_value(llhttp_t *http, const char *at, size_t len)
{
    return 0;
}
static int edc_https_on_message_complete(llhttp_t *http)
{
    return 0;
}

static struct edc_https_request *edc_https_request_new(struct edc_https_client *client, const char *url,
                                                       edc_https_event_fn fn, void *priv)
{
    struct edc_https_request *req = malloc(sizeof(struct edc_https_request));
    req->client = client;
    req->priv = priv;
    req->header.field = NULL;
    req->header.value = NULL;

    llhttp_settings_init(&req->parser_settings);
    req->parser_settings.on_body = edc_https_on_body;
    req->parser_settings.on_header_field = edc_https_on_header_field;
    req->parser_settings.on_header_value = edc_https_on_header_value;
    req->parser_settings.on_message_complete = edc_https_on_message_complete;

    req->url = sdsnew(url);
    yuarel_parse(&req->url_parser, req->url);

    uv_tcp_init(&client->loop, &req->socket);
    return req;
}

static void edc_https_request_free(struct edc_https_request *req)
{
}

void edc_https_client_connect(struct edc_https_client *client, const char *url, edc_https_event_fn fn, void *priv)
{
    struct edc_work *work = malloc(sizeof(struct edc_work));
    work->url = strdup(url);
    work->fn = fn;
    work->priv = priv;
    uv_handle_set_data((uv_handle_t *)&client->work_async, work);
    uv_async_send(&client->work_async);
}

static void edc_https_on_tcp_close(uv_handle_t *handle)
{
    struct edc_https_request *req = (struct edc_https_request *)handle;
    req->fn(req, EDC_HTTPS_EVENT_CLOSE, NULL);
    edc_https_request_free((struct edc_https_request *)handle);
}

static int edc_https_ssl_bio_send(void *ctx, const unsigned char *buf, size_t len);
static int edc_https_ssl_bio_recv(void *ctx, unsigned char *buf, size_t len);

static void edc_https_client_drive_mbedtls_hs(struct edc_https_request *req)
{
    int ret = mbedtls_ssl_handshake(&req->ssl);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        return;
    }

    if (ret != 0)
    {
        char buf[1024];
        mbedtls_strerror(ret, buf, sizeof(buf));
        sds message = sdscatprintf(sdsnew(NULL), "mbedtls handshake error: %s", buf);
        req->fn(req, EDC_HTTPS_EVENT_ERROR, message);
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        return;
    }

    req->fn(req, EDC_HTTPS_EVENT_HS, NULL);
}

static void edc_https_client_drive_mbedtls(struct edc_https_request *req)
{
    while (1)
    {
        if (req->ssl.private_state != MBEDTLS_SSL_HANDSHAKE_OVER)
        {
            edc_https_client_drive_mbedtls_hs(req);
            return;
        }

        unsigned char buf[1024];
        int ret = mbedtls_ssl_read(&req->ssl, buf, sizeof(buf));

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            return;
        }

        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
            log_info("mbedtls ssl read failed: peer close.");
            uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
            return;
        }

        if (ret < 0)
        {
            mbedtls_strerror(ret, (char *)buf, sizeof(buf));
            sds message = sdscatprintf(sdsnew(NULL), "mbedtls ssl read failed: %s", buf);
            req->fn(req, EDC_HTTPS_EVENT_ERROR, message);
            uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
            return;
        }

        llhttp_execute(&req->parser, (void *)buf, ret);
    }
}

static void edc_https_client_on_tcp_connect(uv_connect_t *conn, int status)
{
    struct edc_https_request *req = (struct edc_https_request *)conn->handle;
    if (status < 0)
    {
        sds err_message = sdscatprintf(sdsnew(NULL), "tcp connect failed: %s", uv_strerror(status));
        req->fn(req, EDC_HTTPS_EVENT_ERROR, err_message);
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        return;
    }

    struct edc_https_client *client = req->client;
    mbedtls_ssl_init(&req->ssl);
    mbedtls_ssl_setup(&req->ssl, &client->ssl_conf);
    mbedtls_ssl_set_hostname(&req->ssl, req->url_parser.host);
    mbedtls_ssl_set_bio(&req->ssl, req, edc_https_ssl_bio_send, edc_https_ssl_bio_recv, NULL);
}

static void edc_https_on_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res)
{
    struct edc_https_request *http_req = uv_req_get_data((uv_req_t *)req);
    if (status < 0)
    {
        sds err_message = sdscatprintf(sdsnew(NULL), "tcp connect failed: %s", uv_strerror(status));
        http_req->fn(http_req, EDC_HTTPS_EVENT_ERROR, err_message);
        uv_close((uv_handle_t *)&http_req->socket, edc_https_on_tcp_close);
        return;
    }

    uv_tcp_connect(&http_req->conn, &http_req->socket, res->ai_addr, edc_https_client_on_tcp_connect);
}

static void on_work_async(uv_async_t *handle)
{
    struct edc_work *work = uv_handle_get_data((uv_handle_t *)handle);
    if (!work)
        return;

    // FIXME: queue
    uv_handle_set_data((uv_handle_t *)handle, NULL);

    struct edc_https_client *client = EDC_CONTAINER_OF(handle, struct edc_https_client, work_async);
    struct edc_https_request *http_req = edc_https_request_new(client, work->url, work->fn, work->priv);

    struct addrinfo hints;
    hints.ai_family = PF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = 0;

    uv_getaddrinfo_t *req = malloc(sizeof(uv_getaddrinfo_t));
    uv_req_set_data((uv_req_t *)req, http_req);
    uv_getaddrinfo(&client->loop, req, edc_https_on_resolved, http_req->url_parser.host, "443", &hints);

    free(work->url);
    free(work);
}
