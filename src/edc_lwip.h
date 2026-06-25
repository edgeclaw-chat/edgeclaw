#ifndef __EDC_LWIP_H__
#define __EDC_LWIP_H__

#include <glib.h>

gboolean edc_lwip_init_tap(const char *ipaddr, const char *netmask, const char *gw);
void edc_lwip_init_loopback(void);
gboolean edc_lwip_init_pppos(int fd);
void edc_lwip_shutdown(void);

#endif
