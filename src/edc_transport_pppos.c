#include "edc_transport_pppos.h"

#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "sds.h"
#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct edc_transport_pppos
{
    struct edc_device_transport base;

    int ppp_fd;
    char my_ip[16];
    char peer_ip[16];
    uint16_t port;

    struct netif ni;
    ppp_pcb *ppp;
    volatile int link_up;
    struct netconn *listener;
    struct netconn *conn;
};

static u32_t pppos_out_cb(ppp_pcb *pcb, const void *d, u32_t len, void *ctx)
{
    struct edc_transport_pppos *t = (struct edc_transport_pppos *)ctx;
    (void)pcb;
    ssize_t w = write(t->ppp_fd, d, len);
    return (w < 0) ? 0 : (u32_t)w;
}

static void pppos_status_cb(ppp_pcb *pcb, int code, void *ctx)
{
    struct edc_transport_pppos *t = (struct edc_transport_pppos *)ctx;
    (void)pcb;
    t->link_up = (code == PPPERR_NONE);
}

static void *pppos_reader_thread(void *arg)
{
    struct edc_transport_pppos *t = (struct edc_transport_pppos *)arg;
    char buf[2048];
    ssize_t n;
    while ((n = read(t->ppp_fd, buf, sizeof(buf))) > 0)
        pppos_input_tcpip(t->ppp, buf, (int)n);
    return NULL;
}

static err_t recv_http_request(struct netconn *c, u8_t *buf, size_t *len, size_t cap)
{
    struct netbuf *nb;
    u8_t *p;
    u16_t chunk;
    err_t err;
    int content_length = -1;

    *len = 0;
    while (*len < cap)
    {
        err = netconn_recv(c, &nb);
        if (err != ERR_OK)
            break;
        netbuf_data(nb, (void **)&p, &chunk);
        if (*len + chunk > cap)
            chunk = (u16_t)(cap - *len);
        memcpy(buf + *len, p, chunk);
        *len += chunk;
        buf[*len] = '\0';
        netbuf_delete(nb);

        char *body = strstr((char *)buf, "\r\n\r\n");
        if (!body)
            continue;
        if (content_length < 0)
        {
            char *cl = strcasestr((char *)buf, "Content-Length:");
            content_length = cl ? atoi(cl + 15) : 0;
        }
        if (*len >= (size_t)(body + 4 - (char *)buf) + (size_t)content_length)
            break;
    }
    return (*len > 0) ? ERR_OK : ERR_CLSD;
}

static int transport_pppos_recv(struct edc_device_transport *base, struct edc_device_request *req)
{
    struct edc_transport_pppos *t = (struct edc_transport_pppos *)base;
    struct netconn *c;
    err_t err;

    err = netconn_accept(t->listener, &c);
    if (err != ERR_OK)
    {
        sys_msleep(100);
        return -1;
    }

    u8_t req_buf[131072];
    size_t req_len = 0;
    err = recv_http_request(c, req_buf, &req_len, sizeof(req_buf));
    if (err != ERR_OK)
    {
        netconn_close(c);
        netconn_delete(c);
        return -1;
    }

    req_buf[req_len] = '\0';
    char *body = strstr((char *)req_buf, "\r\n\r\n");
    if (!body)
    {
        netconn_write(c, "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n", 37, NETCONN_COPY);
        netconn_close(c);
        netconn_delete(c);
        return -1;
    }
    body += 4;

    cJSON *req_json = cJSON_Parse(body);
    if (!req_json)
    {
        netconn_write(c, "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n", 37, NETCONN_COPY);
        netconn_close(c);
        netconn_delete(c);
        return -1;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(req_json, "command");
    if (!cmd_item || !cJSON_IsString(cmd_item))
    {
        cJSON_Delete(req_json);
        netconn_write(c, "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n", 37, NETCONN_COPY);
        netconn_close(c);
        netconn_delete(c);
        return -1;
    }

    req->command = strdup(cmd_item->valuestring);
    req->timeout = 0;

    cJSON *timeout_item = cJSON_GetObjectItem(req_json, "timeout");
    if (timeout_item && cJSON_IsNumber(timeout_item) && timeout_item->valueint > 0)
        req->timeout = timeout_item->valueint;

    cJSON_Delete(req_json);

    t->conn = c;
    return 0;
}

static int transport_pppos_send(struct edc_device_transport *base, const struct edc_device_response *resp)
{
    struct edc_transport_pppos *t = (struct edc_transport_pppos *)base;

    if (!t->conn)
        return -1;

    cJSON *resp_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp_json, "exit_code", resp->exit_code);
    cJSON_AddStringToObject(resp_json, "stdout", resp->stdout_buf);
    cJSON_AddStringToObject(resp_json, "stderr", resp->stderr_buf);

    char *resp_str = cJSON_PrintUnformatted(resp_json);
    size_t body_len = strlen(resp_str);

    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "\r\n",
                           body_len);

    netconn_write_partly(t->conn, hdr, (size_t)hdr_len, NETCONN_COPY, NULL);
    netconn_write_partly(t->conn, resp_str, body_len, NETCONN_COPY, NULL);

    cJSON_Delete(resp_json);
    free(resp_str);

    netconn_close(t->conn);
    netconn_delete(t->conn);
    t->conn = NULL;

    fprintf(stderr, "agent: done (exit_code=%d)\n", resp->exit_code);
    return 0;
}

static void transport_pppos_cleanup(struct edc_device_transport *base)
{
    struct edc_transport_pppos *t = (struct edc_transport_pppos *)base;

    if (t->conn)
    {
        netconn_close(t->conn);
        netconn_delete(t->conn);
        t->conn = NULL;
    }
    if (t->listener)
    {
        netconn_close(t->listener);
        netconn_delete(t->listener);
        t->listener = NULL;
    }
    if (t->ppp)
    {
        pppapi_close(t->ppp, 0);
        t->ppp = NULL;
    }
    free(t);
}

struct edc_device_transport *edc_transport_pppos_create(int ppp_fd,
    const char *my_ip, const char *peer_ip, uint16_t port)
{
    struct edc_transport_pppos *t = (struct edc_transport_pppos *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;

    t->base.recv = transport_pppos_recv;
    t->base.send = transport_pppos_send;
    t->base.cleanup = transport_pppos_cleanup;

    t->ppp_fd = ppp_fd;
    strncpy(t->my_ip, my_ip, sizeof(t->my_ip) - 1);
    strncpy(t->peer_ip, peer_ip, sizeof(t->peer_ip) - 1);
    t->port = port;

    ip4_addr_t a, g, m;

    tcpip_init(NULL, NULL);
    srand((unsigned)(sys_now() ^ getpid()));

    ip4addr_aton(my_ip, &a);
    ip4addr_aton(peer_ip, &g);
    IP4_ADDR(&m, 255, 255, 255, 0);
    memset(&t->ni, 0, sizeof(t->ni));
    ip_addr_copy(t->ni.ip_addr, a);
    ip_addr_copy(t->ni.gw, g);
    ip_addr_copy(t->ni.netmask, m);

    t->ppp = pppapi_pppos_create(&t->ni, pppos_out_cb, pppos_status_cb, t);
    if (!t->ppp)
    {
        fprintf(stderr, "agent: PPPoS create failed\n");
        free(t);
        return NULL;
    }
    pppapi_set_default(t->ppp);
    ppp_set_auth(t->ppp, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_ipcp_ouraddr(t->ppp, &a);
    ppp_set_ipcp_hisaddr(t->ppp, &g);
    sys_thread_new("r", (lwip_thread_fn)pppos_reader_thread, t, 0, 1);
    sys_msleep(50);
    pppapi_connect(t->ppp, 0);

    fprintf(stderr, "agent: waiting for PPP link ...\n");
    while (!t->link_up)
        sys_msleep(50);
    fprintf(stderr, "agent: link UP (%s <- %s)\n", my_ip, peer_ip);

    ip_addr_t bind_addr;
    ip4addr_aton(my_ip, &bind_addr);

    t->listener = netconn_new(NETCONN_TCP);
    if (netconn_bind(t->listener, &bind_addr, port) != ERR_OK)
    {
        fprintf(stderr, "agent: bind failed\n");
        pppapi_close(t->ppp, 0);
        free(t);
        return NULL;
    }
    if (netconn_listen(t->listener) != ERR_OK)
    {
        fprintf(stderr, "agent: listen failed\n");
        netconn_delete(t->listener);
        pppapi_close(t->ppp, 0);
        free(t);
        return NULL;
    }
    fprintf(stderr, "agent: listening on %s:%u\n", my_ip, port);

    return &t->base;
}
