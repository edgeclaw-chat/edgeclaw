#include "edc_lwip.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "netif/tapif.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include <unistd.h>

static struct netif lwip_tap_netif;
static gboolean lwip_initialized = FALSE;

static void init_tap_netif(void *arg)
{
    ip4_addr_t ip, nm, g;
    const char *ipaddr = (const char *)arg;
    (void)ipaddr;
    ipaddr = getenv("LWIP_IPADDR");
    if (!ipaddr) ipaddr = "192.168.100.2";
    if (!getenv("PRECONFIGURED_TAPIF")) setenv("PRECONFIGURED_TAPIF", "tap0", 1);
    if (!ip4addr_aton(ipaddr, &ip)) return;
    IP4_ADDR(&nm, 255, 255, 255, 0);
    IP4_ADDR(&g, 192, 168, 100, 1);
    netif_add(&lwip_tap_netif, &ip, &nm, &g, NULL, tapif_init, tcpip_input);
    netif_set_default(&lwip_tap_netif);
    netif_set_up(&lwip_tap_netif);
}

gboolean edc_lwip_init_tap(const char *ipaddr, const char *netmask, const char *gw)
{
    (void)netmask; (void)gw;
    setenv("LWIP_IPADDR", ipaddr ? ipaddr : "192.168.100.2", 1);
    tcpip_init(init_tap_netif, (void *)ipaddr);
    lwip_initialized = TRUE;
    return TRUE;
}

void edc_lwip_init_loopback(void)
{
    tcpip_init(NULL, NULL);
    lwip_initialized = TRUE;
}

/* ---- PPPoS client: connect to a remote device over serial ---- */

static ppp_pcb *g_pppos_pcb;
static int g_pppos_fd;
static volatile int g_pppos_link_up;

static u32_t pppos_out_cb(ppp_pcb *pcb, const void *d, u32_t len, void *ctx)
{
    int fd = *(int *)ctx; (void)pcb;
    ssize_t w = write(fd, d, len);
    return (w < 0) ? 0 : (u32_t)w;
}

static void pppos_status_cb(ppp_pcb *pcb, int code, void *ctx)
{
    (void)pcb; (void)ctx;
    g_pppos_link_up = (code == PPPERR_NONE);
}

static void *pppos_reader(void *arg)
{
    char buf[2048]; ssize_t n; (void)arg;
    while ((n = read(g_pppos_fd, buf, sizeof(buf))) > 0)
        pppos_input_tcpip(g_pppos_pcb, buf, (int)n);
    return NULL;
}

static void init_pppos_netif(void *arg)
{
    struct netif *ni = (struct netif *)arg;
    ip4_addr_t zero, m;

    IP4_ADDR(&zero, 0, 0, 0, 0);
    IP4_ADDR(&m, 255, 255, 255, 0);
    memset(ni, 0, sizeof(*ni));
    ip_addr_copy(ni->ip_addr, zero);
    ip_addr_copy(ni->gw, zero);
    ip_addr_copy(ni->netmask, m);

    g_pppos_pcb = pppapi_pppos_create(ni, pppos_out_cb, pppos_status_cb, &g_pppos_fd);
    if (!g_pppos_pcb) return;
    pppapi_set_default(g_pppos_pcb);
    ppp_set_auth(g_pppos_pcb, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_ipcp_ouraddr(g_pppos_pcb, &zero);   /* 0.0.0.0 = let server assign */
    ppp_set_ipcp_hisaddr(g_pppos_pcb, &zero);   /* 0.0.0.0 = accept whatever server says */
    sys_thread_new("pppos_rd", (lwip_thread_fn)pppos_reader, NULL, 0, 1);
    sys_msleep(50);
    pppapi_connect(g_pppos_pcb, 0);
}

gboolean edc_lwip_init_pppos(int fd)
{
    g_pppos_fd = fd;
    lwip_initialized = TRUE;
    tcpip_init(init_pppos_netif, NULL);

    int timeout = 200;
    while (!g_pppos_link_up && timeout > 0) {
        sys_msleep(50);
        timeout--;
    }
    return g_pppos_link_up;
}

void edc_lwip_shutdown(void)
{
    if (!lwip_initialized) return;
    if (lwip_tap_netif.name[0] != 0) {
        netif_set_down(&lwip_tap_netif);
        netif_remove(&lwip_tap_netif);
    }
    lwip_initialized = FALSE;
}
