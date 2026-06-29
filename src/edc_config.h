#ifndef __BC_CONFIG_H__
#define __BC_CONFIG_H__
#include <glib.h>
#include <stdbool.h>

struct llm_api_conf
{
    char *model;
    char *api_key;
    char *api_url;
};

typedef enum
{
    edc_channel_email,
    edc_channel_telegram,
    edc_channel_feishu,
    edc_channel_qq,
    edc_channel_cli,
} edc_channel_type;

struct channel_conf
{
    struct
    {
        char *imap_host;
        char *smtp_host;
        char *user;
        char *passwd;
        char *target;

    } email;

    struct
    {
        char *bot_key;
        int64_t user_id;
    } telegram;

    struct
    {
        char *user_open_id;
        char *app_id;
        char *app_secret;
    } feishu;

    struct
    {
        char *app_id;
        char *app_secret;
    } qq;

    edc_channel_type type;
};

struct edc_config
{
    char *edc_home;

    struct llm_api_conf llm_api_conf;
    struct channel_conf channel;

    char *workspace;
    bool enable_sandbox;
    char *sandbox_root_real;
    char **sandbox_ro_paths;
    char **sandbox_rw_paths;
    int max_iter;
    int load_n_histories;
    int memory_compress_threshold;
    int tool_max_output_size;

    char *device_pty;
    char *device_ip;
    int device_port;
    int device_baud;
    char *device_transport;
};

extern char *edc_config_resolve_bc_home(void);
extern struct edc_config *edc_config_load(const char *edc_home);
extern void edc_config_free(struct edc_config *config);
#endif
