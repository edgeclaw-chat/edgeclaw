#ifndef __BC_HTTP_ASYNC_H__
#define __BC_HTTP_ASYNC_H__
#include "edc_containers.h"
#include "mongoose.h"
#include <stdbool.h>
#include <uv.h>

typedef void (*network_async_task_fn)(void *);

struct mg_mgr;
typedef struct
{
    uv_thread_t thread;
    struct mg_mgr mgr;
    struct mg_str cert_pem;
    uv_mutex_t mutex;
    EdcQueue task_queue;
    volatile int running;
    void *pending_retries;
} edc_network_async_t;

#define BC_CONN_ERROR (MG_EV_USER + 1)
#define BC_CONN_TIME_OUT (MG_EV_USER + 2)

extern edc_network_async_t *edc_network_async_new(const char *cert_path);
extern void edc_network_async_free(edc_network_async_t *async);
extern void edc_http_async_connect(edc_network_async_t *async, const char *url, const char *proxy,
                                   mg_event_handler_t fn, void *fn_data, int timeout);
extern void edc_wss_async_connect(edc_network_async_t *async, const char *url, mg_event_handler_t fn, void *fn_data,
                                  int timeout);
extern void edc_network_async_close_conn(edc_network_async_t *async, struct mg_connection *c);
extern void edc_network_async_reset_dns(struct mg_mgr *mgr);

#endif
