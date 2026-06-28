#ifndef __BC_INIT_ENV_H__
#define __BC_INIT_ENV_H__

#include <stdbool.h>
#include <stdint.h>

struct edc_init_env_channel_email
{
    char imap_host[256];
    char smtp_host[256];
    char user[256];
    char passwd[256];
    char target[256];
};

struct edc_init_env_channel_telegram
{
    char bot_key[256];
    int64_t user_id;
};

struct edc_init_env_channel_feishu
{
    char app_id[256];
    char app_secret[256];
};

struct edc_init_env_channel_qq
{
    char app_id[256];
    char app_secret[256];
};

struct edc_init_env_result
{
    char api_key[512];
    char api_url[256];
    char model[128];
    int max_iter;
    int load_n_histories;
    int memory_compress_threshold;
    int tool_max_output_size;
    char sandbox_root[512];
    char sandbox_rw_paths[2048];
    char sandbox_ro_paths[2048];

    /* channel type: "none", "email", "telegram", "feishu", "qq" */
    char channel_type[32];

    char device_pty[256];
    char device_ip[64];
    int device_port;
    int device_baud;
    char device_transport[32];

    struct edc_init_env_channel_email email;
    struct edc_init_env_channel_telegram telegram;
    struct edc_init_env_channel_feishu feishu;
    struct edc_init_env_channel_qq qq;
};

bool edc_init_env(const char *edc_home, bool setup);
#endif
