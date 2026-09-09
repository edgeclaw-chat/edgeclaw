#include "edc_https_client.h"
#include "llhttp.h"
#include "log.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "sds.h"
#include "utarray.h"
#include "utlist.h"
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

    struct edc_work *work_queue;
    uv_mutex_t work_mutex;
};

struct edc_work
{
    char *url;
    edc_https_event_fn fn;
    void *priv;
    struct edc_work *next;
};

static void edc_https_client_uv_buf_copy(void *src, const void *dst)
{
    *(uv_buf_t *)src = *(uv_buf_t *)dst;
}

static void edc_https_client_uv_buf_free(void *p)
{
    uv_buf_t *buf = p;
    if (buf->base)
    {
        free(buf->base);
    }
}

static UT_icd ut_uv_buf_icd = {sizeof(struct uv_buf_t), NULL, edc_https_client_uv_buf_copy,
                               edc_https_client_uv_buf_free};

struct edc_https_buf_list
{
    uv_buf_t buf;
    size_t offset;
    struct edc_https_buf_list *next;
};

struct edc_https_buf_list *edc_https_buf_list_node(const uv_buf_t *buf, size_t offset)
{
    struct edc_https_buf_list *node = malloc(sizeof(struct edc_https_buf_list));
    node->buf = *buf;
    node->offset = offset;
    node->next = NULL;
    return node;
}

struct edc_https_request
{
    uv_tcp_t socket;
    uv_connect_t conn;
    mbedtls_ssl_context ssl;
    struct edc_https_buf_list *mbedtls_recv_bufs;
    struct edc_https_buf_list *mbedtls_send_bufs;
    bool writing;

    llhttp_t parser;
    llhttp_settings_t parser_settings;

    struct yuarel url_parser;
    sds url;

    struct edc_https_client *client;
    edc_https_event_fn fn;

    struct edc_https_header header;

    sds current_header_field;
    sds current_header_value;
    bool header_in_field;

    void *priv;
};

static int edc_https_on_header_field(llhttp_t *http, const char *at, size_t len)
{
    struct edc_https_request *req = EDC_CONTAINER_OF(http, struct edc_https_request, parser);

    if (!req->header_in_field && req->current_header_value)
    {
        req->header.field = req->current_header_field;
        req->header.value = req->current_header_value;
        req->fn(req, EDC_HTTPS_EVENT_HEADER, &req->header);
        sdsfree(req->current_header_field);
        sdsfree(req->current_header_value);
        req->current_header_field = NULL;
        req->current_header_value = NULL;
    }

    req->header_in_field = true;
    req->current_header_field = sdscatlen(req->current_header_field ? req->current_header_field : sdsempty(), at, len);
    return 0;
}

static int edc_https_on_header_value(llhttp_t *http, const char *at, size_t len)
{
    struct edc_https_request *req = EDC_CONTAINER_OF(http, struct edc_https_request, parser);

    req->header_in_field = false;
    req->current_header_value = sdscatlen(req->current_header_value ? req->current_header_value : sdsempty(), at, len);
    return 0;
}

static int edc_https_on_headers_complete(llhttp_t *http)
{
    struct edc_https_request *req = EDC_CONTAINER_OF(http, struct edc_https_request, parser);

    if (req->current_header_value)
    {
        req->header.field = req->current_header_field;
        req->header.value = req->current_header_value;
        req->fn(req, EDC_HTTPS_EVENT_HEADER, &req->header);
        sdsfree(req->current_header_field);
        req->current_header_field = NULL;
        sdsfree(req->current_header_value);
        req->current_header_value = NULL;
    }
    else if (req->current_header_field)
    {
        sdsfree(req->current_header_field);
        req->current_header_field = NULL;
    }

    return 0;
}

static int edc_https_on_body(llhttp_t *http, const char *at, size_t length)
{
    struct edc_https_request *req = EDC_CONTAINER_OF(http, struct edc_https_request, parser);

    char *data = malloc(length);
    memcpy(data, at, length);
    uv_buf_t buf = uv_buf_init(data, (unsigned int)length);
    req->fn(req, EDC_HTTPS_EVENT_MSG, &buf);
    /* caller owns buf.base, must free it */
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
    req->fn = fn;
    req->header.field = NULL;
    req->header.value = NULL;
    req->header.next = NULL;

    req->current_header_field = NULL;
    req->current_header_value = NULL;
    req->header_in_field = true;

    llhttp_settings_init(&req->parser_settings);
    req->parser_settings.on_body = edc_https_on_body;
    req->parser_settings.on_header_field = edc_https_on_header_field;
    req->parser_settings.on_header_value = edc_https_on_header_value;
    req->parser_settings.on_headers_complete = edc_https_on_headers_complete;
    req->parser_settings.on_message_complete = edc_https_on_message_complete;

    llhttp_init(&req->parser, HTTP_RESPONSE, &req->parser_settings);

    req->mbedtls_recv_bufs = NULL;
    req->mbedtls_send_bufs = NULL;
    req->writing = false;
    req->url = sdsnew(url);
    if (yuarel_parse(&req->url_parser, req->url) < 0)
    {
        log_error("edc_https_client: invalid url: %s", url);
        sdsfree(req->url);
        free(req);
        return NULL;
    }

    uv_tcp_init(&client->loop, &req->socket);
    return req;
}

static void edc_https_request_free(struct edc_https_request *req)
{
    struct edc_https_buf_list *node = req->mbedtls_recv_bufs;
    while (node)
    {
        struct edc_https_buf_list *next = node->next;
        free(node->buf.base);
        free(node);
        node = next;
    }

    node = req->mbedtls_send_bufs;
    while (node)
    {
        struct edc_https_buf_list *next = node->next;
        free(node->buf.base);
        free(node);
        node = next;
    }

    mbedtls_ssl_free(&req->ssl);
    sdsfree(req->url);
    sdsfree(req->current_header_field);
    sdsfree(req->current_header_value);
    free(req);
}

int edc_https_client_connect(struct edc_https_client *client, const char *url, edc_https_event_fn fn, void *priv)
{
    if (!client || !url || !fn)
    {
        return -1;
    }

    /* Validate the URL up front so that ownership of @p priv is only taken when
     * the request can actually be created. yuarel_parse() is destructive, so it
     * must run on a throwaway copy rather than the caller's string. */
    {
        struct yuarel tmp;
        char *copy = strdup(url);
        if (!copy)
        {
            return -1;
        }
        int invalid = yuarel_parse(&tmp, copy) < 0;
        free(copy);
        if (invalid)
        {
            log_error("edc_https_client: invalid url: %s", url);
            return -1;
        }
    }

    struct edc_work *work = malloc(sizeof(struct edc_work));
    if (!work)
    {
        return -1;
    }
    work->url = strdup(url);
    if (!work->url)
    {
        free(work);
        return -1;
    }
    work->fn = fn;
    work->priv = priv;
    work->next = NULL;

    uv_mutex_lock(&client->work_mutex);
    LL_APPEND(client->work_queue, work);
    uv_mutex_unlock(&client->work_mutex);

    uv_async_send(&client->work_async);
    return 0;
}

static void edc_https_on_tcp_close(uv_handle_t *handle);

const char *edc_https_request_get_host(struct edc_https_request *req)
{
    return req->url_parser.host;
}

const char *edc_https_request_get_path(struct edc_https_request *req)
{
    /* yuarel_parse() leaves path as NULL when the URL has no path component
     * (e.g. "https://host"). Return "" so callers that do "POST /%s" produce a
     * valid request line instead of passing NULL to %s. */
    return req->url_parser.path ? req->url_parser.path : "";
}

void *edc_https_request_get_priv(struct edc_https_request *req)
{
    return req->priv;
}

int edc_https_request_write(struct edc_https_request *req, const unsigned char *buf, size_t len)
{
    size_t written = 0;
    while (written < len)
    {
        int ret = mbedtls_ssl_write(&req->ssl, buf + written, len - written);
        if (ret < 0)
        {
            unsigned char errbuf[1024];
            mbedtls_strerror(ret, (char *)errbuf, sizeof(errbuf));
            sds message = sdscatprintf(sdsnew(NULL), "mbedtls ssl write failed: %s", (char *)errbuf);
            req->fn(req, EDC_HTTPS_EVENT_ERROR, message);
            sdsfree(message);
            uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
            return -1;
        }
        written += (size_t)ret;
    }
    return (int)written;
}

static void edc_https_on_tcp_close(uv_handle_t *handle)
{
    struct edc_https_request *req = (struct edc_https_request *)handle;
    req->fn(req, EDC_HTTPS_EVENT_CLOSE, NULL);
    edc_https_request_free((struct edc_https_request *)handle);
}

static void edc_https_client_drive_mbedtls(struct edc_https_request *req);

static void edc_https_client_on_tcp_write(uv_write_t *write_req, int status)
{
    struct edc_https_request *req = (struct edc_https_request *)write_req->handle;
    struct edc_https_buf_list *node = req->mbedtls_send_bufs;
    req->mbedtls_send_bufs = node->next;
    free(node->buf.base);
    free(node);
    free(write_req);

    if (status < 0)
    {
        sds message = sdscatprintf(sdsnew(NULL), "tcp write failed: %s", uv_strerror(status));
        req->fn(req, EDC_HTTPS_EVENT_ERROR, message);
        sdsfree(message);
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        return;
    }

    if (req->mbedtls_send_bufs)
    {
        uv_write_t *wr = malloc(sizeof(uv_write_t));
        if (uv_write(wr, (uv_stream_t *)&req->socket, &req->mbedtls_send_bufs->buf, 1, edc_https_client_on_tcp_write) <
            0)
        {
            free(wr);
            sds message = sdscatprintf(sdsnew(NULL), "uv_write failed in on_tcp_write");
            req->fn(req, EDC_HTTPS_EVENT_ERROR, message);
            sdsfree(message);
            uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
            return;
        }
    }
    else
    {
        req->writing = false;
        edc_https_client_drive_mbedtls(req);
    }
}

static int edc_https_ssl_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct edc_https_request *req = (struct edc_https_request *)ctx;

    char *data = malloc(len);
    memcpy(data, buf, len);
    uv_buf_t wbuf = uv_buf_init(data, (unsigned int)len);
    /* utlist's LL_APPEND evaluates its `add` argument twice; passing the
     * edc_https_buf_list_node() call directly allocates two nodes and leaks
     * the first one on every send. Hoist the call so the node is created
     * exactly once. */
    struct edc_https_buf_list *node = edc_https_buf_list_node(&wbuf, 0);
    LL_APPEND(req->mbedtls_send_bufs, node);

    if (!req->writing)
    {
        req->writing = true;
        uv_write_t *wr = malloc(sizeof(uv_write_t));
        if (uv_write(wr, (uv_stream_t *)&req->socket, &req->mbedtls_send_bufs->buf, 1, edc_https_client_on_tcp_write) <
            0)
        {
            free(wr);
            req->writing = false;
            sds message = sdscatprintf(sdsnew(NULL), "uv_write failed in bio_send");
            req->fn(req, EDC_HTTPS_EVENT_ERROR, message);
            sdsfree(message);
            uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
            return -1;
        }
    }

    return (int)len;
}

static int edc_https_ssl_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct edc_https_request *req = (struct edc_https_request *)ctx;

    if (req->mbedtls_recv_bufs == NULL)
    {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    size_t total = 0;
    struct edc_https_buf_list *node = req->mbedtls_recv_bufs;

    while (node != NULL && total < len)
    {
        size_t remaining = node->buf.len - node->offset;
        size_t to_copy = (len - total) < remaining ? (len - total) : remaining;

        memcpy(buf + total, node->buf.base + node->offset, to_copy);
        node->offset += to_copy;
        total += to_copy;

        if (node->offset >= node->buf.len)
        {
            struct edc_https_buf_list *old = node;
            node = node->next;
            free(old->buf.base);
            free(old);
        }
        else
        {
            break;
        }
    }

    req->mbedtls_recv_bufs = node;
    return (int)total;
}

static bool edc_https_client_drive_mbedtls_hs(struct edc_https_request *req)
{
    int ret = mbedtls_ssl_handshake(&req->ssl);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        return false;
    }

    if (ret != 0)
    {
        char buf[1024];
        mbedtls_strerror(ret, buf, sizeof(buf));
        sds message = sdscatprintf(sdsnew(NULL), "mbedtls handshake error: %s", buf);
        req->fn(req, EDC_HTTPS_EVENT_ERROR, message);
        sdsfree(message);
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        return false;
    }

    req->fn(req, EDC_HTTPS_EVENT_HS, NULL);
    return true;
}

static bool edc_https_client_drive_mbedtls_read(struct edc_https_request *req)
{
    unsigned char buf[1024];
    int ret = mbedtls_ssl_read(&req->ssl, buf, sizeof(buf));

    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        return false;
    }

    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
        log_info("mbedtls ssl : peer close notify.");
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        return false;
    }

    if (ret < 0)
    {
        mbedtls_strerror(ret, (char *)buf, sizeof(buf));
        sds message = sdscatprintf(sdsnew(NULL), "mbedtls ssl read failed: %s", buf);
        req->fn(req, EDC_HTTPS_EVENT_ERROR, message);
        sdsfree(message);
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        return false;
    }

    llhttp_execute(&req->parser, (void *)buf, ret);
    return true;
}

static void edc_https_client_drive_mbedtls(struct edc_https_request *req)
{
    if (uv_is_closing((uv_handle_t *)&req->socket))
    {
        return;
    }

    while (1)
    {
        if (req->ssl.private_state != MBEDTLS_SSL_HANDSHAKE_OVER)
        {
            if (!edc_https_client_drive_mbedtls_hs(req))
            {
                break;
            }
        }
        else if (!edc_https_client_drive_mbedtls_read(req))
        {
            break;
        }
    }
}

static void edc_https_client_alloc_read_buf(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    *buf = uv_buf_init(malloc(1024 * 64), 64 * 1024);
}

void edc_https_client_on_tcp_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct edc_https_request *req = (struct edc_https_request *)stream;
    if (nread == UV_EOF)
    {
        if (buf->base)
        {
            free(buf->base);
        }
        edc_https_client_drive_mbedtls(req);
        /* drive_mbedtls() may already have closed the handle (e.g. on
         * MBEDTLS_ERR_SSL_CONN_EOF or PEER_CLOSE_NOTIFY); closing twice is UB. */
        if (!uv_is_closing((uv_handle_t *)req))
        {
            uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        }
    }
    else if (nread < 0)
    {
        if (buf->base)
        {
            free(buf->base);
        }
        sds err_message = sdscatprintf(sdsnew(NULL), "tcp read failed: %s", uv_strerror((int)nread));
        req->fn(req, EDC_HTTPS_EVENT_ERROR, err_message);
        sdsfree(err_message);
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
    }
    else
    {
        uv_buf_t actual = uv_buf_init(buf->base, (unsigned int)nread);
        /* See edc_https_ssl_bio_send(): LL_APPEND double-evaluates `add`. */
        struct edc_https_buf_list *node = edc_https_buf_list_node(&actual, 0);
        LL_APPEND(req->mbedtls_recv_bufs, node);
        edc_https_client_drive_mbedtls(req);
    }
}

static void edc_https_client_on_tcp_connect(uv_connect_t *conn, int status)
{
    struct edc_https_request *req = (struct edc_https_request *)conn->handle;
    if (status < 0)
    {
        sds err_message = sdscatprintf(sdsnew(NULL), "tcp connect failed: %s", uv_strerror(status));
        req->fn(req, EDC_HTTPS_EVENT_ERROR, err_message);
        sdsfree(err_message);
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        return;
    }

    struct edc_https_client *client = req->client;

    mbedtls_ssl_init(&req->ssl);
    if (mbedtls_ssl_setup(&req->ssl, &client->ssl_conf) != 0)
    {
        sds err_message = sdscatprintf(sdsnew(NULL), "mbedtls ssl_setup failed");
        req->fn(req, EDC_HTTPS_EVENT_ERROR, err_message);
        sdsfree(err_message);
        uv_close((uv_handle_t *)req, edc_https_on_tcp_close);
        return;
    }
    mbedtls_ssl_set_hostname(&req->ssl, req->url_parser.host);
    mbedtls_ssl_set_bio(&req->ssl, req, edc_https_ssl_bio_send, edc_https_ssl_bio_recv, NULL);
    uv_read_start((uv_stream_t *)req, edc_https_client_alloc_read_buf, edc_https_client_on_tcp_read);
    edc_https_client_drive_mbedtls(req);
}

static void edc_https_on_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res)
{
    struct edc_https_request *http_req = uv_req_get_data((uv_req_t *)req);
    if (status < 0)
    {
        sds err_message = sdscatprintf(sdsnew(NULL), "tcp connect failed: %s", uv_strerror(status));
        http_req->fn(http_req, EDC_HTTPS_EVENT_ERROR, err_message);
        sdsfree(err_message);
        uv_close((uv_handle_t *)&http_req->socket, edc_https_on_tcp_close);
        free(req);
        return;
    }

    uv_tcp_connect(&http_req->conn, &http_req->socket, res->ai_addr, edc_https_client_on_tcp_connect);
    uv_freeaddrinfo(res);
    free(req);
}

static void on_work_async(uv_async_t *handle)
{
    struct edc_https_client *client = EDC_CONTAINER_OF(handle, struct edc_https_client, work_async);

    /* drain work queue */
    while (1)
    {
        uv_mutex_lock(&client->work_mutex);
        struct edc_work *work = client->work_queue;
        if (work)
            LL_DELETE(client->work_queue, work);
        uv_mutex_unlock(&client->work_mutex);

        if (!work)
            break;

        struct edc_https_request *http_req = edc_https_request_new(client, work->url, work->fn, work->priv);
        if (!http_req)
        {
            /* The URL was already validated in edc_https_client_connect(), so this
             * can only be reached on allocation/uv_tcp_init failure. Never invoke
             * fn() with a NULL request (the callback dereferences it). */
            log_error("edc_https_client: failed to create request for %s", work->url);
            free(work->url);
            free(work);
            continue;
        }

        struct addrinfo hints = {0};
        hints.ai_family = PF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = 0;

        uv_getaddrinfo_t *req = malloc(sizeof(uv_getaddrinfo_t));
        uv_req_set_data((uv_req_t *)req, http_req);
        sds port_s = sdscatprintf(sdsempty(), "%d", http_req->url_parser.port ? http_req->url_parser.port : 443);
        uv_getaddrinfo(&client->loop, req, edc_https_on_resolved, http_req->url_parser.host, port_s, &hints);
        sdsfree(port_s);

        free(work->url);
        free(work);
    }
}

static void edc_https_client_thread(void *arg)
{
    struct edc_https_client *client = arg;
    uv_run(&client->loop, UV_RUN_DEFAULT);
}

static void edc_https_on_stop_async(uv_async_t *handle)
{
    struct edc_https_client *client = EDC_CONTAINER_OF(handle, struct edc_https_client, stop_async);
    uv_close((uv_handle_t *)handle, NULL);
    uv_close((uv_handle_t *)&client->work_async, NULL);
    uv_stop(&client->loop);
}

struct edc_https_client *edc_https_client_new(const char *cacert_path)
{

    struct edc_https_client *client = malloc(sizeof(struct edc_https_client));
    uv_loop_init(&client->loop);
    uv_async_init(&client->loop, &client->work_async, on_work_async);
    uv_async_init(&client->loop, &client->stop_async, edc_https_on_stop_async);
    uv_mutex_init(&client->work_mutex);
    client->work_queue = NULL;

    mbedtls_entropy_init(&client->entropy);
    mbedtls_ctr_drbg_init(&client->ctr_drbg);

#define _MBEDTLS_CHECK(__fn)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((ret = (__fn)) != 0)                                                                                       \
        {                                                                                                              \
            mbedtls_strerror(ret, buffer, sizeof(buffer));                                                             \
            log_error(#__fn " failed: %s", buffer);                                                                    \
            free(client);                                                                                              \
            return NULL;                                                                                               \
        }                                                                                                              \
    } while (0)

    int ret = 0;
    char buffer[512];

    _MBEDTLS_CHECK(mbedtls_ctr_drbg_seed(&client->ctr_drbg, mbedtls_entropy_func, &client->entropy, NULL, 0));

    mbedtls_ssl_config_init(&client->ssl_conf);
    _MBEDTLS_CHECK(mbedtls_ssl_config_defaults(&client->ssl_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                               MBEDTLS_SSL_PRESET_DEFAULT));

    mbedtls_ssl_conf_rng(&client->ssl_conf, mbedtls_ctr_drbg_random, &client->ctr_drbg);

    mbedtls_x509_crt_init(&client->cacert);

    if (cacert_path)
    {
        int ret = mbedtls_x509_crt_parse_file(&client->cacert, cacert_path);
        if (ret == 0)
        {
            mbedtls_ssl_conf_ca_chain(&client->ssl_conf, &client->cacert, NULL);
            mbedtls_ssl_conf_authmode(&client->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        }
        else
        {
            char errbuf[256];
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            log_error("edc_https_client: failed to load cacert '%s': %s", cacert_path, errbuf);
            mbedtls_ssl_conf_authmode(&client->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
        }
    }
    else
    {
        mbedtls_ssl_conf_authmode(&client->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    ret = uv_thread_create(&client->thread, edc_https_client_thread, client);
    if (ret < 0)
    {
        log_error("uv_thread_create failed: %s", uv_strerror(ret));
        free(client);
        return NULL;
    }

    return client;
}

static void edc_https_request_cleanup_close(uv_handle_t *handle)
{
    edc_https_request_free((struct edc_https_request *)handle);
}

static void edc_https_client_close_walk(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (handle->type == UV_TCP)
        uv_close(handle, edc_https_request_cleanup_close);
    else
        uv_close(handle, NULL);
}

void edc_https_client_free(struct edc_https_client *client)
{
    uv_async_send(&client->stop_async);
    uv_thread_join(&client->thread);

    uv_walk(&client->loop, edc_https_client_close_walk, NULL);
    uv_run(&client->loop, UV_RUN_NOWAIT);

    uv_loop_close(&client->loop);

    /* free any remaining work queue items */
    {
        struct edc_work *work, *tmp;
        LL_FOREACH_SAFE(client->work_queue, work, tmp)
        {
            free(work->url);
            free(work);
        }
    }
    uv_mutex_destroy(&client->work_mutex);

    mbedtls_ssl_config_free(&client->ssl_conf);
    mbedtls_ctr_drbg_free(&client->ctr_drbg);
    mbedtls_entropy_free(&client->entropy);
    mbedtls_x509_crt_free(&client->cacert);

    free(client);
}
