#ifndef __EDC_TRANSPORT_SOCKET_H__
#define __EDC_TRANSPORT_SOCKET_H__

#include "edc_device_agent.h"

struct edc_device_transport *edc_transport_socket_create(const char *bind_ip, uint16_t port);

#endif
