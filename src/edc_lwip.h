#ifndef __EDC_LWIP_H__
#define __EDC_LWIP_H__

#include <glib.h>

gboolean edc_lwip_init_tap(const gchar *ipaddr, const gchar *netmask, const gchar *gw);
void edc_lwip_init_loopback(void);
void edc_lwip_shutdown(void);

#endif
