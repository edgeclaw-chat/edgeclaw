#ifndef __EDC_PPPOS_AGENT_H__
#define __EDC_PPPOS_AGENT_H__

#include <stdint.h>

void edc_pppos_agent_run(int ppp_fd, const char *my_ip, const char *peer_ip, uint16_t port);

#endif
