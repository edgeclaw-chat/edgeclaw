#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/wait.h>
#include <signal.h>
#include <pty.h>

#define SERVER_IP "192.168.2.1"
#define CLIENT_IP "192.168.2.2"
#define HTTP_PORT 8080

static ppp_pcb *ppp;
static int ppp_fd = -1;
static volatile int ppp_link_up = 0;

static u32_t my_pppos_output_cb(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    int fd = *(int *)ctx;
    (void)pcb;
    ssize_t wrote = write(fd, data, len);
    if (wrote < 0) return 0;
    return (u32_t)wrote;
}

static void my_link_status_cb(ppp_pcb *pcb, int err_code, void *ctx)
{
    (void)pcb;
    (void)ctx;
    if (err_code == PPPERR_NONE) {
        printf("  PPP link UP!\n");
        ppp_link_up = 1;
    } else {
        printf("  PPP link down, code=%d\n", err_code);
    }
}

static void *ppp_read_thread(void *arg)
{
    char buf[2048];
    ssize_t n;

    (void)arg;
    while (1) {
        n = read(ppp_fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0) fprintf(stderr, "  [PPP read exit: %s]\n", strerror(errno));
            break;
        }
        pppos_input_tcpip(ppp, buf, (int)n);
    }
    return NULL;
}

static void init_pppos(struct netif *ppp_netif, int fd,
                       const char *my_ip, const char *peer_ip)
{
    ip4_addr_t ip, nm, gw;

    printf("  PPPoS init on fd %d (IP=%s peer=%s)\n", fd, my_ip, peer_ip);

    ppp_fd = fd;

    ip4addr_aton(my_ip, &ip);
    ip4addr_aton(peer_ip, &gw);
    IP4_ADDR(&nm, 255, 255, 255, 0);

    memset(ppp_netif, 0, sizeof(*ppp_netif));
    ip_addr_copy(ppp_netif->ip_addr, ip);
    ip_addr_copy(ppp_netif->gw, gw);
    ip_addr_copy(ppp_netif->netmask, nm);

    ppp = pppapi_pppos_create(ppp_netif,
                              my_pppos_output_cb,
                              my_link_status_cb,
                              &ppp_fd);
    if (!ppp) {
        printf("  PPPoS create failed!\n");
        return;
    }

    pppapi_set_default(ppp);
    ppp_set_auth(ppp, PPPAUTHTYPE_NONE, NULL, NULL);

    {
        ip4_addr_t my_addr, peer_addr;
        ip4addr_aton(my_ip, &my_addr);
        ip4addr_aton(peer_ip, &peer_addr);
        ppp_set_ipcp_ouraddr(ppp, &my_addr);
        ppp_set_ipcp_hisaddr(ppp, &peer_addr);
    }

    sys_thread_new("ppp_rd", (lwip_thread_fn)ppp_read_thread, NULL, 0, 1);
    sys_msleep(50);

    pppapi_connect(ppp, 0);
}

static void run_http_server(const char *ip)
{
    struct netconn *listener, *client;
    struct netbuf *inbuf;
    err_t err;
    const char *body = "Hello via lwIP PPPoS!";
    char response[512];
    size_t written;
    ip_addr_t bind_addr;

    if (!ip4addr_aton(ip, &bind_addr)) return;

    listener = netconn_new(NETCONN_TCP);
    if (!listener) { printf("  server: netconn_new failed\n"); return; }

    err = netconn_bind(listener, &bind_addr, HTTP_PORT);
    if (err != ERR_OK) { printf("  server: bind failed (%d)\n", err); netconn_delete(listener); return; }

    netconn_listen(listener);
    printf("  HTTP server on %s:%d\n", ip, HTTP_PORT);

    err = netconn_accept(listener, &client);
    if (err != ERR_OK) { printf("  server: accept failed (%d)\n", err); netconn_delete(listener); return; }

    err = netconn_recv(client, &inbuf);
    if (err == ERR_OK && inbuf) {
        u16_t len;
        void *data;
        netbuf_data(inbuf, &data, &len);
        printf("  Server RX: %.*s\n", len, (char *)data);
        netbuf_delete(inbuf);
    }

    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n%s",
             strlen(body), body);

    netconn_write_partly(client, response, strlen(response), NETCONN_COPY, &written);
    printf("  Server TX: %zu bytes\n", written);

    netconn_close(client);
    netconn_delete(client);
    netconn_close(listener);
    netconn_delete(listener);
}

static void run_http_client(const char *server_ip)
{
    struct netconn *conn;
    struct netbuf *inbuf;
    err_t err;
    char request[256];
    size_t written;
    ip_addr_t addr;

    if (!ip4addr_aton(server_ip, &addr)) return;

    conn = netconn_new(NETCONN_TCP);
    if (!conn) { printf("  client: netconn_new failed\n"); return; }

    printf("  Client connecting to %s:%d ...\n", server_ip, HTTP_PORT);
    err = netconn_connect(conn, &addr, HTTP_PORT);
    if (err != ERR_OK) {
        printf("  client: connect failed (%d)\n", err);
        netconn_delete(conn);
        return;
    }
    printf("  Client connected.\n");

    snprintf(request, sizeof(request),
             "GET / HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
             server_ip, HTTP_PORT);

    netconn_write_partly(conn, request, strlen(request), NETCONN_COPY, &written);
    printf("  Client TX: %zu bytes\n", written);

    err = netconn_recv(conn, &inbuf);
    if (err == ERR_OK && inbuf) {
        u16_t len;
        void *data;
        netbuf_data(inbuf, &data, &len);
        printf("  Client RX: %.*s\n", len, (char *)data);
        netbuf_delete(inbuf);
        printf("  PASS\n");
    } else {
        printf("  client: recv failed (%d)\n", err);
    }

    netconn_close(conn);
    netconn_delete(conn);
}

static void child_process(int fd)
{
    struct netif ppp_netif;

    tcpip_init(NULL, NULL);
    srand((unsigned)(sys_now() ^ getpid()));

    printf("\n[SERVER]\n");
    init_pppos(&ppp_netif, fd, SERVER_IP, CLIENT_IP);

    {
        int timeout = 100;
        while (!ppp_link_up && timeout > 0) {
            sys_msleep(100);
            timeout--;
        }
    }
    if (!ppp_link_up) {
        printf("[SERVER] PPP link failed to come up\n");
        exit(1);
    }

    run_http_server(SERVER_IP);
    printf("[SERVER] Done.\n");
    exit(0);
}

static void parent_process(int fd)
{
    struct netif ppp_netif;

    tcpip_init(NULL, NULL);
    srand((unsigned)(sys_now() ^ getpid()));

    printf("\n[CLIENT]\n");
    init_pppos(&ppp_netif, fd, CLIENT_IP, SERVER_IP);

    {
        int timeout = 100;
        printf("[CLIENT] Waiting for PPP link ...\n");
        while (!ppp_link_up && timeout > 0) {
            sys_msleep(100);
            timeout--;
        }
    }
    if (!ppp_link_up) {
        printf("[CLIENT] PPP link failed to come up\n");
        wait(NULL);
        exit(1);
    }

    sys_msleep(500);
    run_http_client(SERVER_IP);

    printf("[CLIENT] Waiting for server ...\n");
    wait(NULL);
    printf("[CLIENT] Done.\n");
}

int main(void)
{
    int master_fd, slave_fd;
    pid_t pid;

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("=== lwIP PPPoS HTTP Test ===\n\n");

    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0) {
        perror("openpty");
        return 1;
    }

    {
        struct termios t;
        tcgetattr(slave_fd, &t);
        cfmakeraw(&t);
        tcsetattr(slave_fd, TCSANOW, &t);
    }

    signal(SIGPIPE, SIG_IGN);
    printf("PTY: master=%d slave=%d\n", master_fd, slave_fd);

    pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        close(master_fd);
        child_process(slave_fd);
        return 0;
    } else {
        close(slave_fd);
        parent_process(master_fd);
    }

    return 0;
}
