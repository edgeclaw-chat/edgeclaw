#ifndef __EDC_LWIP_H__
#define __EDC_LWIP_H__

#include <stdbool.h>

void edc_lwip_init_loopback(void);
bool edc_lwip_init_pppos(int fd);
bool edc_lwip_init_pppos_path(const char *path, int baud);
void edc_lwip_shutdown(void);

#endif
