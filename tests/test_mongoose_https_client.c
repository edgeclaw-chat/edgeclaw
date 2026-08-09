#include "mongoose.h"
#include "src/edc_network_async.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#define TRUE true
#define FALSE false

// 1. 定义事件处理回调
static void fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        printf("收到响应: %.*s\n", (int)hm->body.len, hm->body.buf);
    }
    else if (ev == MG_EV_ERROR)
    {
        printf("错误: %s\n", (char *)ev_data);
    }
}

static volatile bool running = TRUE;

static void signal_handler(int sig)
{
    if (sig == SIGINT)
    {
        running = FALSE;
    }
}

int main(void)
{
    signal(SIGINT, signal_handler);

    edc_network_async_t *async = edc_network_async_new("/etc/ssl/cert.pem");
    // char *proxy = g_getenv("http_proxy");
    char *proxy = NULL;
    async->mgr.dns4.url = "udp://192.168.31.1:53";
    edc_http_async_connect(async, "https://httpbin.org/get", NULL, fn, proxy, 60);

    while (running)
    {
        sleep(5);
    }

    edc_network_async_free(async);
    return 0;
}
