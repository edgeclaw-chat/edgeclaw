#ifndef __BC_CONFIG_H__
#define __BC_CONFIG_H__
#include "sds.h"
#include <stdbool.h>
#include <stdint.h>

struct llm_api_conf
{
    sds model;
    sds api_key;
    sds api_url;
};

typedef enum
{
    edc_channel_cli,
    edc_channel_adapter,
} edc_channel_type;

struct channel_conf
{
    edc_channel_type type;
};

struct edc_config
{
    sds edc_home;

    struct llm_api_conf llm_api_conf;
    struct channel_conf channel;

    sds workspace;
    bool enable_sandbox;
    sds sandbox_root_real;
    sds *sandbox_ro_paths;
    sds *sandbox_rw_paths;
    int max_iter;
    int load_n_histories;
    int memory_compress_threshold;
    int tool_max_output_size;

    sds device_pty;
    sds device_ip;
    int device_port;
    int device_baud;
    sds device_transport;
};

extern sds edc_config_resolve_bc_home(void);
extern struct edc_config *edc_config_load(const char *edc_home);
extern void edc_config_free(struct edc_config *config);
#endif
