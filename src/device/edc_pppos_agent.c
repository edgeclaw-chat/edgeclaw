#include "edc_pppos_agent.h"
#include "edc_device_agent.h"
#include "edc_transport_pppos.h"

void edc_pppos_agent_run(int ppp_fd, const char *my_ip, const char *peer_ip, uint16_t port)
{
    struct edc_device_transport *t = edc_transport_pppos_create(ppp_fd, my_ip, peer_ip, port);
    if (!t)
        return;
    edc_device_agent_run(t);
    t->cleanup(t);
}
