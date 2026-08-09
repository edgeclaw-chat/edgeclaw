#include "edc_socket_agent.h"
#include "edc_device_agent.h"
#include "edc_transport_socket.h"

void edc_socket_agent_run(const char *bind_ip, uint16_t port)
{
    struct edc_device_transport *t = edc_transport_socket_create(bind_ip, port);
    if (!t)
        return;
    edc_device_agent_run(t);
    t->cleanup(t);
}
