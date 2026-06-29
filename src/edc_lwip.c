#include "edc_lwip.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#ifdef __linux__
#include "netif/tapif.h"
#endif
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#ifdef __linux__
static struct netif lwip_tap_netif;
#endif
static struct netif lwip_pppos_netif;
static bool lwip_initialized = FALSE;

#ifdef __linux__
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

bool edc_lwip_init_tap(const char *ipaddr, const char *netmask, const char *gw)
{
    (void)netmask; (void)gw;
    setenv("LWIP_IPADDR", ipaddr ? ipaddr : "192.168.100.2", 1);
    tcpip_init(init_tap_netif, (void *)ipaddr);
    lwip_initialized = TRUE;
    return TRUE;
}
#endif

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

bool edc_lwip_init_pppos(int fd)
{
    struct netif *ni = &lwip_pppos_netif;
    ip4_addr_t my_addr, peer_addr, nm;

    g_pppos_fd = fd;
    lwip_initialized = TRUE;
    tcpip_init(NULL, NULL);

    IP4_ADDR(&my_addr, 0, 0, 0, 0);
    IP4_ADDR(&peer_addr, 0, 0, 0, 0);
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
    return g_pppos_link_up;
}

static speed_t int_to_baud(int baud)
{
    switch (baud) {
    case 0:        return B0;
    case 50:       return B50;
    case 75:       return B75;
    case 110:      return B110;
    case 134:      return B134;
    case 150:      return B150;
    case 200:      return B200;
    case 300:      return B300;
    case 600:      return B600;
    case 1200:     return B1200;
    case 1800:     return B1800;
    case 2400:     return B2400;
    case 4800:     return B4800;
    case 9600:     return B9600;
    case 19200:    return B19200;
    case 38400:    return B38400;
    case 57600:    return B57600;
    case 115200:   return B115200;
    case 230400:   return B230400;
#ifdef B460800
    case 460800:   return B460800;
#endif
#ifdef B500000
    case 500000:   return B500000;
#endif
#ifdef B576000
    case 576000:   return B576000;
#endif
#ifdef B921600
    case 921600:   return B921600;
#endif
#ifdef B1000000
    case 1000000:  return B1000000;
#endif
#ifdef B1152000
    case 1152000:  return B1152000;
#endif
#ifdef B1500000
    case 1500000:  return B1500000;
#endif
#ifdef B2000000
    case 2000000:  return B2000000;
#endif
#ifdef B2500000
    case 2500000:  return B2500000;
#endif
#ifdef B3000000
    case 3000000:  return B3000000;
#endif
#ifdef B3500000
    case 3500000:  return B3500000;
#endif
#ifdef B4000000
    case 4000000:  return B4000000;
#endif
    default:       return B115200;
    }
}

bool edc_lwip_init_pppos_path(const char *path, int baud)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) return FALSE;

    struct termios t;
    tcgetattr(fd, &t);
    t.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= (tcflag_t)~OPOST;
    t.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
    t.c_cflag |= CS8;
    cfsetispeed(&t, int_to_baud(baud));
    cfsetospeed(&t, int_to_baud(baud));
    tcsetattr(fd, TCSANOW, &t);

    return edc_lwip_init_pppos(fd);
}

void edc_lwip_shutdown(void)
{
    if (!lwip_initialized) return;
#ifdef __linux__
    if (lwip_tap_netif.name[0] != 0) {
        netif_set_down(&lwip_tap_netif);
        netif_remove(&lwip_tap_netif);
    }
#endif
    if (lwip_pppos_netif.name[0] != 0) {
        netif_set_down(&lwip_pppos_netif);
        netif_remove(&lwip_pppos_netif);
    }
    lwip_initialized = FALSE;
}
