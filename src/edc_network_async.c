#include "edc_strutil.h"
#include "edc_network_async.h"
#include "log.h"
#include <resolv.h>

typedef struct
{
    GSourceFunc fn;
    void* data;
} edc_network_async_task_t;

struct edc_network_async_request
{
    enum
    {
        HTTP,
        WSS
    } type;

    uint64_t create_time;
    int retries;
    mg_event_handler_t fn;
    void *fn_data;
    bool connected;
    int timeout;

    edc_network_async_t *network;
    const char *url;
    const char *proxy;

    /* linked list for pending retry tracking */
    struct edc_network_async_request *pending_next;
};

static void edc_network_async_request_free(struct edc_network_async_request *req);

static void edc_network_async_enqueue(edc_network_async_t *async, GSourceFunc fn, void* data)
{
    edc_network_async_task_t *task = g_new0(edc_network_async_task_t, 1);
    task->fn = fn;
    task->data = data;
    g_mutex_lock(&async->mutex);
    g_queue_push_tail(&async->task_queue, task);
    g_mutex_unlock(&async->mutex);
}

static void edc_network_async_pending_add(edc_network_async_t *async, struct edc_network_async_request *req)
{
    req->pending_next = async->pending_retries;
    async->pending_retries = req;
}

static void edc_network_async_pending_remove(edc_network_async_t *async, struct edc_network_async_request *req)
{
    struct edc_network_async_request **p = (struct edc_network_async_request **)&async->pending_retries;
    while (*p)
    {
        if (*p == req)
        {
            *p = req->pending_next;
            return;
        }
        p = &(*p)->pending_next;
    }
}

static void* network_async_thread(void* user_data)
{
    edc_network_async_t *async = (edc_network_async_t *)user_data;
    while (g_atomic_int_get(&async->running))
    {
        for (;;)
        {
            edc_network_async_task_t *task = NULL;
            g_mutex_lock(&async->mutex);
            task = g_queue_pop_head(&async->task_queue);
            g_mutex_unlock(&async->mutex);
            if (!task)
                break;
            task->fn(task->data);
            free(task);
        }
        mg_mgr_poll(&async->mgr, 100);
    }
    return NULL;
}

static char *edc_network_async_get_dns()
{
    struct __res_state dns_state;
    memset(&dns_state, 0, sizeof(struct __res_state));

    if (res_ninit(&dns_state) != 0)
    {
        log_error("network async: system error (errno): %s\n", strerror(errno));
        if (dns_state.res_h_errno != 0)
        {
            log_error("network async: resolver error: %s\n", hstrerror(dns_state.res_h_errno));
        }
        res_nclose(&dns_state);
        return NULL;
    }

    char *result = NULL;

#if defined(__APPLE__)
    union res_sockaddr_union servers[MAXNS];
    memset(&servers, 0, sizeof(servers));
    int count = res_getservers(&dns_state, servers, MAXNS);
    for (int i = 0; i < count; i++)
    {
        if (servers[i].sin.sin_family == AF_INET6)
        {
            continue;
        }
        else
        {
            result = edc_strdup_printf("udp://%s:53", inet_ntoa(servers[i].sin.sin_addr));
            break;
        }
    }
#else
    for (int i = 0; i < dns_state.nscount; i++)
    {
        if (dns_state._u._ext.nsaddrs[i] != NULL)
        {
            struct sockaddr *sa = (struct sockaddr *)dns_state._u._ext.nsaddrs[i];
            if (sa->sa_family == AF_INET6)
            {
                continue;
            }
        }
        else
        {
            struct sockaddr_in *addr = &dns_state.nsaddr_list[i];
            result = edc_strdup_printf("udp://%s:53", inet_ntoa(addr->sin_addr));
            break;
        }
    }
#endif

    res_nclose(&dns_state);
    return result;
}

edc_network_async_t *edc_network_async_new(const char *cert_path)
{
    edc_network_async_t *async = g_new0(edc_network_async_t, 1);
    mg_mgr_init(&async->mgr);
    async->mgr.dnstimeout = 5000;
    mg_log_set(MG_LL_ERROR);

    char *dns = edc_network_async_get_dns();
    if (dns)
    {
        async->mgr.dns4.url = dns;
    }
    log_info("network async init with dns: %s", async->mgr.dns4.url);

    async->cert_pem = mg_file_read(&mg_fs_posix, cert_path);

    g_mutex_init(&async->mutex);
    g_queue_init(&async->task_queue);
    async->running = TRUE;
    async->thread = g_thread_new("edc_network_async", (GThreadFunc)network_async_thread, async);

    return async;
}

void edc_network_async_free(edc_network_async_t *async)
{
    g_atomic_int_set(&async->running, FALSE);
    g_thread_join(async->thread);

    edc_network_async_task_t *task;
    while ((task = g_queue_pop_head(&async->task_queue)))
    {
        free(task);
    }
    g_mutex_clear(&async->mutex);

    free((char *)async->mgr.dns4.url);
    mg_mgr_free(&async->mgr);
    mg_free(async->cert_pem.buf);

    /* Free any retry requests whose timers never fired */
    struct edc_network_async_request *req = async->pending_retries;
    while (req)
    {
        struct edc_network_async_request *next = req->pending_next;
        edc_network_async_request_free(req);
        req = next;
    }

    free(async);
}


static struct edc_network_async_request *edc_http_async_request_new(edc_network_async_t *async, const char *url,
                                                                  const char *proxy, mg_event_handler_t fn,
                                                                  void *fn_data, int timeout)
{
    struct edc_network_async_request *req = g_new0(struct edc_network_async_request, 1);
    req->type = HTTP;
    req->network = async;
    req->url = strdup(url);
    req->create_time = mg_millis();
    req->retries = 3;
    req->fn = fn;
    req->fn_data = fn_data;
    req->proxy = proxy ? strdup(proxy) : NULL;
    req->timeout = timeout;
    req->connected = FALSE;

    return req;
}

static struct edc_network_async_request *edc_wss_async_request_new(edc_network_async_t *async, const char *url,
                                                                 mg_event_handler_t fn, void *fn_data, int timeout)
{
    struct edc_network_async_request *req = g_new0(struct edc_network_async_request, 1);
    req->type = WSS;
    req->network = async;
    req->url = strdup(url);
    req->create_time = mg_millis();
    req->retries = 3;
    req->fn = fn;
    req->fn_data = fn_data;
    req->proxy = NULL;
    req->timeout = timeout;
    req->connected = FALSE;

    return req;
}

static void edc_network_async_request_free(struct edc_network_async_request *req)
{
    if (req->url)
    {
        free((char *)req->url);
    }

    if (req->proxy)
    {
        free((char *)req->proxy);
    }

    free(req);
}

static void edc_network_async_fn_forward(struct mg_connection *c, int ev, void *ev_data)
{
    struct edc_network_async_request *req = (struct edc_network_async_request *)c->fn_data;
    bool was_closing = c->is_closing;
    c->fn_data = req->fn_data;
    req->fn(c, ev, ev_data);
    c->fn_data = req;
    if (c->is_closing && !was_closing)
    {
        req->retries = 0;
    }
}

static bool mg_http_connect_wrapper(void* data);
static bool mg_wss_connect_wrapper(void* data);
void edc_network_async_reset_dns(struct mg_mgr *mgr)
{
    if (mgr->dns4.c != NULL)
    {
        mgr->dns4.c->is_closing = 1;
    }
    if (mgr->dns6.c != NULL)
    {
        mgr->dns6.c->is_closing = 1;
    }
}

static void edc_network_async_retry_timer_cb(void *arg)
{
    struct edc_network_async_request *req = (struct edc_network_async_request *)arg;
    edc_network_async_pending_remove(req->network, req);
    if (req->type == WSS)
    {
        mg_wss_connect_wrapper(req);
    }
    else
    {
        mg_http_connect_wrapper(req);
    }
}

static void edc_network_async_retry(struct edc_network_async_request *req)
{
    static int retry_interval_sec[] = {3, 5, 7};
    int secs = retry_interval_sec[3 - req->retries];
    edc_network_async_reset_dns(&req->network->mgr);
    mg_timer_add(&req->network->mgr, (uint64_t)secs * 1000, 0, edc_network_async_retry_timer_cb, req);
    edc_network_async_pending_add(req->network, req);
    req->retries -= 1;
}

// 负责处理proxy、超时、重试
static void edc_network_async_conn_fn(struct mg_connection *c, int ev, void *ev_data)
{
    struct edc_network_async_request *req = (struct edc_network_async_request *)c->fn_data;
    if (ev == MG_EV_ERROR)
    {
        edc_network_async_reset_dns(&req->network->mgr);
        if (req->retries > 0)
        {
            log_error("http async conn %s error: %s, retry.", req->url, (const char *)ev_data);
        }
        else
        {
            log_error("http async conn %s error: %s", req->url, (const char *)ev_data);
            edc_network_async_fn_forward(c, ev, ev_data);
        }

        c->is_closing = 1;
    }
    else if (ev == MG_EV_OPEN)
    {
        // c->is_hexdumping = 1;
        req->create_time = mg_millis();
    }
    else if (ev == MG_EV_POLL)
    {
        uint64_t delta = mg_millis() - req->create_time;
        delta = delta / 1000;
        if (req->timeout > 0 && delta > req->timeout)
        {
            if (req->retries > 0)
            {
                log_error("http async conn %s timeout, retry.", req->url);
                c->is_closing = 1;
            }
            else
            {
                log_error("http async conn %s timeout.", req->url);
                edc_network_async_fn_forward(c, BC_CONN_TIME_OUT, "timeout");
                c->is_closing = 1;
            }
        }
        else
        {
            edc_network_async_fn_forward(c, ev, ev_data);
        }
    }
    else if (ev == MG_EV_CONNECT)
    {
        struct mg_str host = mg_url_host(req->url);
        unsigned short port = mg_url_port(req->url);
        if (c->is_tls)
        {
            struct mg_tls_opts opts = {.ca = req->network->cert_pem, .name = host};
            mg_tls_init(c, &opts);
        }

        if (req->proxy)
        {
            mg_printf(c, "CONNECT %.*s:%d HTTP/1.1\r\nHOST %.*s:%d\r\n\r\n", (int)host.len, host.buf, port,
                      (int)host.len, host.buf, port);
        }

        req->connected = TRUE;
        edc_network_async_fn_forward(c, ev, ev_data);
    }
    else if (ev == MG_EV_TLS_HS)
    {
        edc_network_async_fn_forward(c, ev, ev_data);
    }
    else if (req && req->connected && req->proxy && ev == MG_EV_READ)
    {
        struct mg_http_message hm;
        int n = mg_http_parse((char *)c->recv.buf, c->recv.len, &hm);
        if (n > 0)
        {
            req->connected = TRUE;
            mg_iobuf_del(&c->recv, 0, n);

            struct mg_str host = mg_url_host(req->url);
            struct mg_tls_opts opts = {.ca = req->network->cert_pem, .name = host};
            mg_tls_init(c, &opts);

            mg_tls_handshake(c);
        }
    }
    else if (ev == MG_EV_CLOSE)
    {
        if (req->retries > 0)
        {
            edc_network_async_retry(req);
        }
        else
        {
            edc_network_async_fn_forward(c, ev, ev_data);
            edc_network_async_request_free(req);
        }

        c->is_closing = 1;
        c->fn_data = NULL;
    }
    else if (ev == MG_EV_HTTP_HDRS)
    {
        edc_network_async_fn_forward(c, ev, ev_data);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        edc_network_async_fn_forward(c, ev, ev_data);
    }
    else if (ev == MG_EV_WS_OPEN)
    {
        edc_network_async_fn_forward(c, ev, ev_data);
    }
    else if (ev == MG_EV_WS_MSG)
    {
        edc_network_async_fn_forward(c, ev, ev_data);
    }
}

static bool mg_http_connect_wrapper(void* data)
{
    struct edc_network_async_request *req = (struct edc_network_async_request *)data;
    const char *direct = req->proxy ? req->proxy : req->url;
    struct mg_str host = mg_url_host(direct);
    unsigned short port = mg_url_port(direct);
    const char *proto = mg_url_is_ssl(direct) ? "https" : "http";

    char *url = edc_strdup_printf("%s://%.*s:%d", proto, (int)host.len, host.buf, (int)port);

    struct mg_connection *c = mg_http_connect(&req->network->mgr, url, edc_network_async_conn_fn, req);
    free(url);

    if (c == NULL)
    {
        req->fn(NULL, BC_CONN_ERROR, req->fn_data);
        edc_network_async_request_free(req);
        log_error("connect to %s failed.", req->url);
    }

    return G_SOURCE_REMOVE;
}

static bool mg_wss_connect_wrapper(void* data)
{
    struct edc_network_async_request *req = (struct edc_network_async_request *)data;
    struct mg_connection *c = mg_ws_connect(&req->network->mgr, req->url, edc_network_async_conn_fn, req, NULL);

    if (c == NULL)
    {
        req->fn(NULL, BC_CONN_ERROR, req->fn_data);
        edc_network_async_request_free(req);
        log_error("connect to %s failed.", req->url);
    }

    return G_SOURCE_REMOVE;
}

void edc_http_async_connect(edc_network_async_t *async, const char *url, const char *proxy, mg_event_handler_t fn,
                           void *fn_data, int timeout)
{
    struct edc_network_async_request *req = edc_http_async_request_new(async, url, proxy, fn, fn_data, timeout);
    edc_network_async_enqueue(async, (GSourceFunc)mg_http_connect_wrapper, req);
}

void edc_wss_async_connect(edc_network_async_t *async, const char *url, mg_event_handler_t fn, void *fn_data,
                          int timeout)
{
    struct edc_network_async_request *req = edc_wss_async_request_new(async, url, fn, fn_data, timeout);
    edc_network_async_enqueue(async, (GSourceFunc)mg_wss_connect_wrapper, req);
}

typedef struct
{
    edc_network_async_t *async;
    struct mg_connection *conn;
} edc_network_async_close_task_t;

static bool edc_network_close_conn_wrapper(void *data)
{
    edc_network_async_close_task_t *task = data;
    /* Verify the connection hasn't been freed by Mongoose yet */
    for (struct mg_connection *c = task->async->mgr.conns; c != NULL; c = c->next)
    {
        if (c == task->conn)
        {
            c->is_closing = 1;
            ((struct edc_network_async_request *)c->fn_data)->retries = 0;
            break;
        }
    }
    free(task);
    return G_SOURCE_REMOVE;
}

void edc_network_async_close_conn(edc_network_async_t *async, struct mg_connection *c)
{
    edc_network_async_close_task_t *task = g_new0(edc_network_async_close_task_t, 1);
    task->async = async;
    task->conn = c;
    edc_network_async_enqueue(async, (GSourceFunc)edc_network_close_conn_wrapper, task);
}
