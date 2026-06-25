#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/wait.h>
#include <signal.h>
#include <pty.h>

#define SERVER_IP  "192.168.3.1"
#define CLIENT_IP  "192.168.3.2"
#define PORT        9000
#define ITERATIONS  300

static ppp_pcb *ppp;
static int ppp_fd = -1;
static volatile int ppp_link_up = 0;

static u32_t output_cb(ppp_pcb *pcb, const void *d, u32_t len, void *ctx)
{
    int fd = *(int *)ctx;
    (void)pcb;
    ssize_t w = write(fd, d, len);
    return (w < 0) ? 0 : (u32_t)w;
}

static void status_cb(ppp_pcb *pcb, int code, void *ctx)
{
    (void)pcb; (void)ctx;
    ppp_link_up = (code == PPPERR_NONE);
}

static void *reader(void *arg)
{
    char buf[2048];
    ssize_t n;
    (void)arg;
    while ((n = read(ppp_fd, buf, sizeof(buf))) > 0) {
        pppos_input_tcpip(ppp, buf, (int)n);
    }
    return NULL;
}

static void init_pppos(struct netif *ni, int fd,
                        const char *my, const char *peer)
{
    ip4_addr_t a, g, m;
    ppp_fd = fd;
    ip4addr_aton(my, &a);
    ip4addr_aton(peer, &g);
    IP4_ADDR(&m, 255, 255, 255, 0);
    memset(ni, 0, sizeof(*ni));
    ip_addr_copy(ni->ip_addr, a);
    ip_addr_copy(ni->gw, g);
    ip_addr_copy(ni->netmask, m);

    ppp = pppapi_pppos_create(ni, output_cb, status_cb, &ppp_fd);
    pppapi_set_default(ppp);
    ppp_set_auth(ppp, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_ipcp_ouraddr(ppp, &a);
    ppp_set_ipcp_hisaddr(ppp, &g);

    sys_thread_new("r", (lwip_thread_fn)reader, NULL, 0, 1);
    sys_msleep(30);
    pppapi_connect(ppp, 0);
}

/* ---- server: echo payload back to client ---- */
static err_t netconn_read_exact(struct netconn *c, u8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        struct netbuf *nb;
        u8_t *p;
        u16_t chunk;
        err_t err = netconn_recv(c, &nb);
        if (err != ERR_OK) return err;
        netbuf_data(nb, (void **)&p, &chunk);
        if (total + chunk > len) chunk = (u16_t)(len - total);
        memcpy(buf + total, p, chunk);
        total += chunk;
        netbuf_delete(nb);
    }
    return ERR_OK;
}

static void server_thread(void *arg)
{
    struct netconn *ln, *c;
    u8_t *buf;
    err_t err;
    u32_t pkt_len;
    int conn_count = 0;

    (void)arg;

    buf = (u8_t *)malloc(65536);
    ln = netconn_new(NETCONN_TCP);
    ip_addr_t bind_addr;
    ip4addr_aton(SERVER_IP, &bind_addr);
    netconn_bind(ln, &bind_addr, PORT);
    netconn_listen(ln);

    /* accept 3 connections (one per benchmark) */
    for (conn_count = 0; conn_count < 3; conn_count++) {
        err = netconn_accept(ln, &c);
        if (err != ERR_OK) break;

        /* handle all requests on this connection */
        while (1) {
            err = netconn_read_exact(c, (u8_t *)&pkt_len, 4);
            if (err != ERR_OK) break;

            err = netconn_read_exact(c, buf, pkt_len);
            if (err != ERR_OK) break;

            netconn_write(c, buf, pkt_len, NETCONN_COPY);
        }

        netconn_close(c);
        netconn_delete(c);
    }

    netconn_close(ln);
    netconn_delete(ln);
    free(buf);
    exit(0);
}

/* ---- client: send payload, measure time, verify ---- */
static void run_bench_one_conn(const char *label, int payload_sz)
{
    struct netconn *c;
    err_t err;
    ip_addr_t addr;
    u8_t *tx_data, *rx_data;
    u32_t *tx_seq, *rx_seq;
    int i, ok = 0, fail = 0;
    u32_t total_bytes = 0;
    u64_t total_us = 0;
    u64_t min_us = ~0ULL, max_us = 0;

    tx_data = (u8_t *)malloc(payload_sz);
    rx_data = (u8_t *)malloc(payload_sz);

    printf("\n  %s: single conn, %d iterations x %d bytes\n", label, ITERATIONS, payload_sz);

    ip4addr_aton(SERVER_IP, &addr);
    c = netconn_new(NETCONN_TCP);
    err = netconn_connect(c, &addr, PORT);
    if (err != ERR_OK) {
        printf("  CONNECT FAILED\n");
        free(tx_data); free(rx_data);
        return;
    }

    {
        u32_t t_start = sys_now();
        for (i = 0; i < ITERATIONS; i++) {
            tx_seq = (u32_t *)tx_data;
            *tx_seq = (u32_t)i;
            memset(tx_data + 4, (u8_t)i, payload_sz - 4);

            u32_t pkt_len = payload_sz;
            u32_t t0 = sys_now();

            netconn_write(c, &pkt_len, 4, NETCONN_COPY);
            netconn_write(c, tx_data, payload_sz, NETCONN_COPY);
            err = netconn_read_exact(c, rx_data, payload_sz);
            u32_t t1 = sys_now();

            if (err != ERR_OK) { fail++; break; }

            rx_seq = (u32_t *)rx_data;
            if (*tx_seq != *rx_seq || memcmp(tx_data + 4, rx_data + 4, (size_t)payload_sz - 4) != 0) {
                fail++; break;
            }

            u32_t elapsed_ms = t1 - t0;
            u64_t elapsed_us = elapsed_ms == 0 ? 1 : (u64_t)elapsed_ms * 1000ULL;

            if (i % 100 == 0) {
                printf("  [%d/%d] last=%ums\n", i, ITERATIONS, elapsed_ms);
            }

            total_bytes += payload_sz * 2;
            total_us += elapsed_us;
            if (elapsed_us < min_us) min_us = elapsed_us;
            if (elapsed_us > max_us) max_us = elapsed_us;
            ok++;
        }
        u32_t t_end = sys_now();
        printf("  [%s] OK=%d FAIL=%d time=%ums\n", label, ok, fail, t_end - t_start);
    }

    netconn_close(c);
    netconn_delete(c);
    if (ok > 0) {
        printf("  Latency min/avg/max: %" PRIu64 " / %.0f / %" PRIu64 " us\n",
               min_us, (double)total_us / ok, max_us);
        printf("  Throughput: %.2f MB/s (bidirectional)\n",
               (double)total_bytes / (double)total_us);
    }

    free(tx_data);
    free(rx_data);
}

static void client_run(void)
{
    run_bench_one_conn("512B", 512);
    run_bench_one_conn("4KB", 4096);
    run_bench_one_conn("16KB", 16384);
}

static void server(int fd)
{
    struct netif ni;
    tcpip_init(NULL, NULL);
    srand((unsigned)(sys_now() ^ getpid()));
    init_pppos(&ni, fd, SERVER_IP, CLIENT_IP);
    while (!ppp_link_up) sys_msleep(50);
    server_thread(NULL);
}

static void client(int fd)
{
    struct netif ni;
    tcpip_init(NULL, NULL);
    srand((unsigned)(sys_now() ^ getpid()));
    init_pppos(&ni, fd, CLIENT_IP, SERVER_IP);
    printf("  Waiting for PPP link ...\n");
    while (!ppp_link_up) sys_msleep(50);
    printf("  Link UP.\n");
    sys_msleep(300);
    client_run();
    wait(NULL);
}

int main(void)
{
    int m, s;
    pid_t p;
    struct termios t;

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("=== lwIP PPPoS Stress Test ===\n");

    openpty(&m, &s, NULL, NULL, NULL);
    tcgetattr(s, &t); cfmakeraw(&t); tcsetattr(s, TCSANOW, &t);
    signal(SIGPIPE, SIG_IGN);

    p = fork();
    if (p == 0) { close(m); server(s); }
    else       { close(s); client(m); }

    return 0;
}
