#ifndef __EDC_DEVICE_AGENT_H__
#define __EDC_DEVICE_AGENT_H__

#include <stdint.h>

struct edc_device_request
{
    char *command;
    int timeout;
};

struct edc_device_response
{
    int exit_code;
    char *stdout_buf;
    char *stderr_buf;
};

struct edc_device_transport
{
    int (*recv)(struct edc_device_transport *t, struct edc_device_request *req);
    int (*send)(struct edc_device_transport *t, const struct edc_device_response *resp);
    void (*cleanup)(struct edc_device_transport *t);
};

void edc_device_agent_run(struct edc_device_transport *transport);

#endif
