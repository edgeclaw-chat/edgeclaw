#ifndef __BC_CONFIG_H__
#define __BC_CONFIG_H__
#include <glib.h>

struct llm_api_conf
{
    gchar *model;
    gchar *api_key;
    gchar *api_url;
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
        gchar *imap_host;
        gchar *smtp_host;
        gchar *user;
        gchar *passwd;
        gchar *target;

    } email;

    struct
    {
        gchar *bot_key;
        gint64 user_id;
    } telegram;

    struct
    {
        gchar *user_open_id;
        gchar *app_id;
        gchar *app_secret;
    } feishu;

    struct
    {
        gchar *app_id;
        gchar *app_secret;
    } qq;

    edc_channel_type type;
};

struct edc_config
{
    gchar *edc_home;

    struct llm_api_conf llm_api_conf;
    struct channel_conf channel;

    gchar *workspace;
    gboolean enable_sandbox;
    gchar *sandbox_root_real;
    gchar **sandbox_ro_paths;
    gchar **sandbox_rw_paths;
    int max_iter;
    int load_n_histories;
    int memory_compress_threshold;
    int tool_max_output_size;
};

extern gchar *edc_config_resolve_bc_home(void);
extern struct edc_config *edc_config_load(const gchar *edc_home);
extern void edc_config_free(struct edc_config *config);
#endif
