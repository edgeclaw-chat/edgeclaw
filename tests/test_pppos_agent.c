#include "edc_pppos_agent.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/wait.h>
#include <signal.h>
/* openpty() is declared in <util.h> on macOS (in libSystem) and in <pty.h> on Linux
 * (in libutil). Same function, same signature; only the header and the library differ. */
#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

#define AGENT_IP   "192.168.4.1"
#define CLIENT_IP  "192.168.4.2"
#define PORT       5000

static ppp_pcb *cl_ppp;
static int cl_fd;
static volatile int cl_link_up;

static u32_t cl_out_cb(ppp_pcb *pcb, const void *d, u32_t len, void *ctx)
{
    int fd = *(int *)ctx; (void)pcb;
    ssize_t w = write(fd, d, len);
    return (w < 0) ? 0 : (u32_t)w;
}

static void cl_status_cb(ppp_pcb *pcb, int code, void *ctx)
{
    (void)pcb; (void)ctx;
    cl_link_up = (code == PPPERR_NONE);
}

static void *cl_reader(void *arg)
{
    char buf[2048]; ssize_t n; (void)arg;
    while ((n = read(cl_fd, buf, sizeof(buf))) > 0)
        pppos_input_tcpip(cl_ppp, buf, (int)n);
    return NULL;
}

static void client_run(int fd, const char *cmd)
{
    struct netif ni;
    ip4_addr_t a, g, m;
    ip_addr_t srv;
    struct netconn *c;
    err_t err;

    cl_fd = fd;
    tcpip_init(NULL, NULL);
    srand((unsigned)(sys_now() ^ getpid()));

    ip4addr_aton(CLIENT_IP, &a);
    ip4addr_aton(AGENT_IP, &g);
    IP4_ADDR(&m, 255, 255, 255, 0);
    memset(&ni, 0, sizeof(ni));
    ip_addr_copy(ni.ip_addr, a);
    ip_addr_copy(ni.gw, g);
    ip_addr_copy(ni.netmask, m);

    cl_ppp = pppapi_pppos_create(&ni, cl_out_cb, cl_status_cb, &cl_fd);
    pppapi_set_default(cl_ppp);
    ppp_set_auth(cl_ppp, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_ipcp_ouraddr(cl_ppp, &a);
    ppp_set_ipcp_hisaddr(cl_ppp, &g);
    sys_thread_new("r2", (lwip_thread_fn)cl_reader, NULL, 0, 1);
    sys_msleep(50);
    pppapi_connect(cl_ppp, 0);

    printf("Waiting for PPP link ...\n");
    while (!cl_link_up) sys_msleep(50);
    printf("Link UP.\n");
    sys_msleep(500);

    ip4addr_aton(AGENT_IP, &srv);
    c = netconn_new(NETCONN_TCP);
    err = netconn_connect(c, &srv, PORT);
    if (err != ERR_OK) { printf("FAIL: connect: %d\n", err); netconn_delete(c); exit(1); }

    /* build JSON request */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "command", cmd);
    char *req_str = cJSON_PrintUnformatted(req);

    char http_req[4096];
    int req_hdr_len = snprintf(http_req, sizeof(http_req),
        "POST /exec HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        AGENT_IP, PORT, strlen(req_str));

    size_t total_len = (size_t)req_hdr_len + strlen(req_str);
    if (total_len <= sizeof(http_req) - 1) {
        memcpy(http_req + req_hdr_len, req_str, strlen(req_str));
        netconn_write_partly(c, http_req, total_len, NETCONN_COPY, NULL);
    } else {
        netconn_write_partly(c, http_req, (size_t)req_hdr_len, NETCONN_COPY, NULL);
        netconn_write_partly(c, req_str, strlen(req_str), NETCONN_COPY, NULL);
    }

    cJSON_Delete(req);
    free(req_str);

    /* read HTTP response */
    u8_t resp_buf[65536];
    size_t resp_len = 0;
    {
        struct netbuf *nb; u8_t *p; u16_t chunk;
        while ((err = netconn_recv(c, &nb)) == ERR_OK) {
            netbuf_data(nb, (void **)&p, &chunk);
            if (resp_len + chunk > sizeof(resp_buf) - 1)
                chunk = (u16_t)(sizeof(resp_buf) - 1 - resp_len);
            memcpy(resp_buf + resp_len, p, chunk);
            resp_len += chunk;
            netbuf_delete(nb);
        }
    }
    resp_buf[resp_len] = '\0';
    netconn_close(c);
    netconn_delete(c);

    char *body = strstr((char *)resp_buf, "\r\n\r\n");
    printf("\n--- command: %s ---\n", cmd);

    if (body) {
        body += 4;
        cJSON *resp_json = cJSON_Parse(body);
        if (resp_json) {
            cJSON *ec = cJSON_GetObjectItem(resp_json, "exit_code");
            cJSON *so = cJSON_GetObjectItem(resp_json, "stdout");
            cJSON *se = cJSON_GetObjectItem(resp_json, "stderr");

            printf("exit_code: %d\n", ec ? ec->valueint : -1);
            if (so && cJSON_IsString(so) && so->valuestring[0])
                printf("stdout:\n%s", so->valuestring);
            if (se && cJSON_IsString(se) && se->valuestring[0])
                printf("stderr:\n%s", se->valuestring);
            cJSON_Delete(resp_json);
        } else {
            printf("(invalid JSON response)\n%s\n", body);
        }
    }

    printf("Done.\n");
}

int main(int argc, char **argv)
{
    int m, s;
    pid_t pid;
    struct termios t;

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    const char *cmd = (argc > 1) ? argv[1] : "id; uname -a";
    printf("=== lwIP PPPoS JSON/HTTP Agent ===\n\n");

    openpty(&m, &s, NULL, NULL, NULL);
    tcgetattr(s, &t); cfmakeraw(&t); tcsetattr(s, TCSANOW, &t);
    signal(SIGPIPE, SIG_IGN);

    pid = fork();
    if (pid == 0) { close(m); edc_pppos_agent_run(s, AGENT_IP, CLIENT_IP, PORT); return 0; }

    close(s);
    client_run(m, cmd);
    kill(pid, SIGTERM);
    wait(NULL);
    return 0;
}
