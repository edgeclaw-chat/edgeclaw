#include "edc_lwip.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"
#include "netif/tapif.h"

static struct netif lwip_netif;

gboolean edc_lwip_init(const gchar *ipaddr, const gchar *netmask, const gchar *gw)
{
    ip4_addr_t ip, nm, g;

    lwip_init();

    if (!ipaddr) {
        ipaddr = "192.168.1.100";
    }
    if (!netmask) {
        netmask = "255.255.255.0";
    }
    if (!gw) {
        gw = "192.168.1.1";
    }

    if (!ip4addr_aton(ipaddr, &ip) || !ip4addr_aton(netmask, &nm) || !ip4addr_aton(gw, &g)) {
        return FALSE;
    }

    netif_add(&lwip_netif, &ip, &nm, &g, NULL, tapif_init, tcpip_input);
    netif_set_default(&lwip_netif);
    netif_set_up(&lwip_netif);

    return TRUE;
}

void edc_lwip_shutdown(void)
{
    netif_set_down(&lwip_netif);
    netif_remove(&lwip_netif);
}
