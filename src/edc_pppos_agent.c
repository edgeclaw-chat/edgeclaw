#include "edc_pppos_agent.h"

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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CMD_TIMEOUT_SEC 30

static ppp_pcb *g_ppp;
static int g_ppp_fd;
static volatile int g_link_up;

static u32_t out_cb(ppp_pcb *pcb, const void *d, u32_t len, void *ctx)
{
    int fd = *(int *)ctx;
    (void)pcb;
    ssize_t w = write(fd, d, len);
    return (w < 0) ? 0 : (u32_t)w;
}

static void status_cb(ppp_pcb *pcb, int code, void *ctx)
{
    (void)pcb;
    (void)ctx;
    g_link_up = (code == PPPERR_NONE);
}

static void *reader(void *arg)
{
    char buf[2048];
    ssize_t n;
    (void)arg;
    while ((n = read(g_ppp_fd, buf, sizeof(buf))) > 0)
        pppos_input_tcpip(g_ppp, buf, (int)n);
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

static int exec_cmd(const char *cmd, sds *out, sds *err, int *exit_code, int timeout_sec)
{
    int out_pipe[2], err_pipe[2];
    pid_t pid;

    *out = sdsempty();
    *err = sdsempty();
    *exit_code = -1;

    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0)
        return -1;

    pid = fork();
    if (pid < 0)
    {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        setpgid(0, 0);
#ifdef __ANDROID__
        execl("/system/bin/sh", "sh", "-c", cmd, (char *)NULL);
#else
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
#endif
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    setpgid(pid, 0);

    fd_set rfds;
    int max_fd = (out_pipe[0] > err_pipe[0]) ? out_pipe[0] : err_pipe[0];
    int done_out = 0, done_err = 0;

    while (!done_out || !done_err)
    {
        struct timeval tv;
        struct timeval *tvp = NULL;
        if (timeout_sec > 0)
        {
            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;
            tvp = &tv;
        }

        FD_ZERO(&rfds);
        if (!done_out)
            FD_SET(out_pipe[0], &rfds);
        if (!done_err)
            FD_SET(err_pipe[0], &rfds);

        int ret = select(max_fd + 1, &rfds, NULL, NULL, tvp);
        if (ret < 0)
            break;
        if (ret == 0)
        {
            kill(-pid, SIGKILL);
            *err = sdscat(*err, "\n[command timed out]");
            break;
        }

        char buf[1024];
        ssize_t n;
        if (!done_out && FD_ISSET(out_pipe[0], &rfds))
        {
            n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0)
                *out = sdscatlen(*out, buf, (size_t)n);
            else
                done_out = 1;
        }
        if (!done_err && FD_ISSET(err_pipe[0], &rfds))
        {
            n = read(err_pipe[0], buf, sizeof(buf));
            if (n > 0)
                *err = sdscatlen(*err, buf, (size_t)n);
            else
                done_err = 1;
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

void edc_pppos_agent_run(int ppp_fd, const char *my_ip, const char *peer_ip, uint16_t port)
{
    struct netif ni;
    ip4_addr_t a, g, m;
    ip_addr_t bind_addr;

    g_ppp_fd = ppp_fd;
    tcpip_init(NULL, NULL);
    srand((unsigned)(sys_now() ^ getpid()));

    ip4addr_aton(my_ip, &a);
    ip4addr_aton(peer_ip, &g);
    IP4_ADDR(&m, 255, 255, 255, 0);
    memset(&ni, 0, sizeof(ni));
    ip_addr_copy(ni.ip_addr, a);
    ip_addr_copy(ni.gw, g);
    ip_addr_copy(ni.netmask, m);

    g_ppp = pppapi_pppos_create(&ni, out_cb, status_cb, &g_ppp_fd);
    if (!g_ppp)
    {
        fprintf(stderr, "agent: PPPoS create failed\n");
        return;
    }
    pppapi_set_default(g_ppp);
    ppp_set_auth(g_ppp, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_ipcp_ouraddr(g_ppp, &a);
    ppp_set_ipcp_hisaddr(g_ppp, &g);
    sys_thread_new("r", (lwip_thread_fn)reader, NULL, 0, 1);
    sys_msleep(50);
    pppapi_connect(g_ppp, 0);

    fprintf(stderr, "agent: waiting for PPP link ...\n");
    while (!g_link_up)
        sys_msleep(50);
    fprintf(stderr, "agent: link UP (%s <- %s)\n", my_ip, peer_ip);
    ip4addr_aton(my_ip, &bind_addr);

    struct netconn *ln = netconn_new(NETCONN_TCP);
    if (netconn_bind(ln, &bind_addr, port) != ERR_OK)
    {
        fprintf(stderr, "agent: bind failed\n");
        return;
    }
    if (netconn_listen(ln) != ERR_OK)
    {
        fprintf(stderr, "agent: listen failed\n");
        return;
    }
    fprintf(stderr, "agent: listening on %s:%u\n", my_ip, port);

    while (1)
    {
        struct netconn *c;
        err_t err;

        err = netconn_accept(ln, &c);
        if (err != ERR_OK)
        {
            sys_msleep(100);
            continue;
        }

        u8_t req_buf[131072];
        size_t req_len = 0;
        err = recv_http_request(c, req_buf, &req_len, sizeof(req_buf));
        if (err != ERR_OK)
        {
            netconn_close(c);
            netconn_delete(c);
            continue;
        }

        req_buf[req_len] = '\0';
        char *body = strstr((char *)req_buf, "\r\n\r\n");
        if (!body)
        {
            netconn_write(c, "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n", 37, NETCONN_COPY);
            netconn_close(c);
            netconn_delete(c);
            continue;
        }
        body += 4;

        cJSON *req_json = cJSON_Parse(body);
        if (!req_json)
        {
            netconn_write(c, "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n", 37, NETCONN_COPY);
            netconn_close(c);
            netconn_delete(c);
            continue;
        }

        cJSON *cmd_item = cJSON_GetObjectItem(req_json, "command");
        if (!cmd_item || !cJSON_IsString(cmd_item))
        {
            cJSON_Delete(req_json);
            netconn_write(c, "HTTP/1.1 400\r\nContent-Length: 0\r\n\r\n", 37, NETCONN_COPY);
            netconn_close(c);
            netconn_delete(c);
            continue;
        }

        const char *command = cmd_item->valuestring;
        int cmd_timeout = CMD_TIMEOUT_SEC;
        cJSON *timeout_item = cJSON_GetObjectItem(req_json, "timeout");
        if (timeout_item && cJSON_IsNumber(timeout_item) && timeout_item->valueint > 0)
            cmd_timeout = timeout_item->valueint;

        fprintf(stderr, "agent: exec: %s (timeout %ds)\n", command, cmd_timeout);

        sds out_buf, err_buf;
        int exit_code;
        exec_cmd(command, &out_buf, &err_buf, &exit_code, cmd_timeout);

        cJSON *resp_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp_json, "exit_code", exit_code);
        cJSON_AddStringToObject(resp_json, "stdout", out_buf);
        cJSON_AddStringToObject(resp_json, "stderr", err_buf);

        char *resp_str = cJSON_PrintUnformatted(resp_json);
        size_t body_len = strlen(resp_str);

        char hdr[256];
        int hdr_len = snprintf(hdr, sizeof(hdr),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n"
                               "\r\n",
                               body_len);

        netconn_write_partly(c, hdr, (size_t)hdr_len, NETCONN_COPY, NULL);
        netconn_write_partly(c, resp_str, body_len, NETCONN_COPY, NULL);

        cJSON_Delete(req_json);
        cJSON_Delete(resp_json);
        free(resp_str);
        sdsfree(out_buf);
        sdsfree(err_buf);

        netconn_close(c);
        netconn_delete(c);
    }
}
