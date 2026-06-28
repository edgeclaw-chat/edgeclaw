#ifndef __EDC_TRANSPORT_PPPOS_H__
#define __EDC_TRANSPORT_PPPOS_H__

#include "edc_device_agent.h"

struct edc_device_transport *edc_transport_pppos_create(int ppp_fd,
    const char *my_ip, const char *peer_ip, uint16_t port);

#endif
