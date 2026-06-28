#ifndef __EDC_DEVICE_AGENT_H__
#define __EDC_DEVICE_AGENT_H__

#include <stddef.h>
#include <stdint.h>

enum edc_device_request_type
{
    REQ_EXEC = 0,
    REQ_FILE_UPLOAD = 1,
    REQ_FILE_DOWNLOAD = 2,
};

struct edc_device_request
{
    int type;
    int timeout;

    char *command;

    char *file_path;
    char *file_data;
    size_t file_data_len;
    int file_mode;
    size_t file_offset;
    size_t file_length;
};

struct edc_device_response
{
    int type;

    int exit_code;
    char *stdout_buf;
    char *stderr_buf;

    int file_status;
    size_t file_size;
    size_t file_offset;
    size_t file_length;
};

struct edc_device_transport
{
    int (*recv)(struct edc_device_transport *t, struct edc_device_request *req);
    int (*send)(struct edc_device_transport *t, const struct edc_device_response *resp);
    void (*cleanup)(struct edc_device_transport *t);
};

void edc_device_agent_run(struct edc_device_transport *transport);

#endif
