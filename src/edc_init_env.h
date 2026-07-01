#ifndef __BC_INIT_ENV_H__
#define __BC_INIT_ENV_H__

#include "sds.h"
#include <stdbool.h>
#include <stdint.h>

struct edc_init_env_channel_email
{
    sds imap_host;
    sds smtp_host;
    sds user;
    sds passwd;
    sds target;
};

struct edc_init_env_channel_telegram
{
    sds bot_key;
    int64_t user_id;
};

struct edc_init_env_channel_feishu
{
    sds app_id;
    sds app_secret;
};

struct edc_init_env_channel_qq
{
    sds app_id;
    sds app_secret;
};

struct edc_init_env_result
{
    sds api_key;
    sds api_url;
    sds model;
    int max_iter;
    int load_n_histories;
    int memory_compress_threshold;
    int tool_max_output_size;
    sds sandbox_root;
    sds sandbox_rw_paths;
    sds sandbox_ro_paths;

    /* channel type: "none", "email", "telegram", "feishu", "qq" */
    sds channel_type;

    sds device_pty;
    sds device_ip;
    int device_port;
    int device_baud;
    sds device_transport;

    struct edc_init_env_channel_email email;
    struct edc_init_env_channel_telegram telegram;
    struct edc_init_env_channel_feishu feishu;
    struct edc_init_env_channel_qq qq;
};

void edc_init_env_result_free(struct edc_init_env_result *result);
bool edc_init_env(const char *edc_home, bool setup);
#endif
