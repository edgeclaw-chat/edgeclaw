#ifndef __BC_INIT_ENV_H__
#define __BC_INIT_ENV_H__

#include "sds.h"
#include <stdbool.h>
#include <stdint.h>

struct edc_init_env_result
{
    sds api_key;
    sds api_url;
    sds model;
    int max_iter;
    int load_n_histories;
    int memory_compress_threshold;
    int group_fold_msg_threshold;
    int tool_max_output_size;
    sds sandbox_root;
    sds sandbox_rw_paths;
    sds sandbox_ro_paths;

    sds device_pty;
    sds device_ip;
    int device_port;
    int device_baud;
    sds device_transport;
};

void edc_init_env_result_free(struct edc_init_env_result *result);
bool edc_init_env(const char *edc_home, bool setup);
#endif
