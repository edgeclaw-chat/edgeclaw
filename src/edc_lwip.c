#include "edc_lwip.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "netif/tapif.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

static struct netif lwip_tap_netif;
static struct netif lwip_pppos_netif;
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

gboolean edc_lwip_init_pppos(int fd)
{
    struct netif *ni = &lwip_pppos_netif;
    ip4_addr_t my_addr, peer_addr, nm;

    g_pppos_fd = fd;
    lwip_initialized = TRUE;
    tcpip_init(NULL, NULL);

    IP4_ADDR(&my_addr, 192, 168, 4, 2);
    IP4_ADDR(&peer_addr, 192, 168, 4, 1);
    IP4_ADDR(&nm, 255, 255, 255, 0);
    memset(ni, 0, sizeof(*ni));
    ip_addr_copy(ni->ip_addr, my_addr);
    ip_addr_copy(ni->gw, peer_addr);
    ip_addr_copy(ni->netmask, nm);

    g_pppos_pcb = pppapi_pppos_create(ni, pppos_out_cb, pppos_status_cb, &g_pppos_fd);
    if (!g_pppos_pcb) return FALSE;
    pppapi_set_default(g_pppos_pcb);
    ppp_set_auth(g_pppos_pcb, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_ipcp_ouraddr(g_pppos_pcb, &my_addr);
    ppp_set_ipcp_hisaddr(g_pppos_pcb, &peer_addr);
    sys_thread_new("pppos_rd", (lwip_thread_fn)pppos_reader, NULL, 0, 1);
    sys_msleep(50);
    pppapi_connect(g_pppos_pcb, 0);

    int timeout = 200;
    while (!g_pppos_link_up && timeout > 0) {
        sys_msleep(50);
        timeout--;
    }
    if (g_pppos_link_up)
        sys_msleep(500);
    return g_pppos_link_up;
}

gboolean edc_lwip_init_pppos_path(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) return FALSE;

    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    tcsetattr(fd, TCSANOW, &t);

    return edc_lwip_init_pppos(fd);
}

void edc_lwip_shutdown(void)
{
    if (!lwip_initialized) return;
    if (lwip_tap_netif.name[0] != 0) {
        netif_set_down(&lwip_tap_netif);
        netif_remove(&lwip_tap_netif);
    }
    if (lwip_pppos_netif.name[0] != 0) {
        netif_set_down(&lwip_pppos_netif);
        netif_remove(&lwip_pppos_netif);
    }
    lwip_initialized = FALSE;
}
