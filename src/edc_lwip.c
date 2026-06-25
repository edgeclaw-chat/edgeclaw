#include "edc_lwip.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "netif/tapif.h"

static struct netif lwip_tap_netif;
static gboolean lwip_initialized = FALSE;

static void init_tap_netif(void *arg)
{
    ip4_addr_t ip, nm, g;
    const gchar *ipaddr = (const gchar *)arg;

    (void)ipaddr;
    ipaddr = getenv("LWIP_IPADDR");
    if (!ipaddr)
    {
        ipaddr = "192.168.100.2";
    }

    if (!getenv("PRECONFIGURED_TAPIF"))
    {
        setenv("PRECONFIGURED_TAPIF", "tap0", 1);
    }

    if (!ip4addr_aton(ipaddr, &ip))
    {
        return;
    }
    IP4_ADDR(&nm, 255, 255, 255, 0);
    IP4_ADDR(&g, 192, 168, 100, 1);

    netif_add(&lwip_tap_netif, &ip, &nm, &g, NULL, tapif_init, tcpip_input);
    netif_set_default(&lwip_tap_netif);
    netif_set_up(&lwip_tap_netif);
}

gboolean edc_lwip_init_tap(const gchar *ipaddr, const gchar *netmask, const gchar *gw)
{
    (void)netmask;
    (void)gw;
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

void edc_lwip_shutdown(void)
{
    if (!lwip_initialized)
    {
        return;
    }
    if (lwip_tap_netif.name[0] != 0)
    {
        netif_set_down(&lwip_tap_netif);
        netif_remove(&lwip_tap_netif);
    }
    lwip_initialized = FALSE;
}
