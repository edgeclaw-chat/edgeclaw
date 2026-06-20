#ifndef __BC_CHANNEL_ACTOR_H__
#define __BC_CHANNEL_ACTOR_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_network_async.h"
#include "cJSON.h"
#include <pthread.h>
#include <termios.h>
struct edc_channel
{
    edc_channel_type type;
    union {
        struct
        {
            gchar *title;
            gchar *message_id;
            gchar *reply_to;
            gchar *refs;
        } email;
        struct
        {
            gchar *chat_id;
        } telegram;

        struct
        {
            gchar *chat_type;
            gchar *chat_id;
            gchar *message_id;
            gchar *user_open_id;
        } feishu;

        struct
        {
            gchar *message_id;
            gchar *author_id;
            int64_t seq_id;
        } qq;
    };
};

struct edc_channel_message
{
    gchar *content;
    gboolean channel_ref;
    struct edc_channel *channel;
};

typedef struct
{
    struct channel_conf config;
    edc_network_async_t *network;
    edc_actor_t *observer;
    GHashTable *handlers;
    union {
        struct
        {
            gboolean polling;
            gint64 update_id;
            GSource *poll_source;
        } telegram;

        struct
        {
            gchar *tenant_access_token;
            int64_t expire;
            gchar *wss_url;
            int32_t wss_service_id;
            GSource *expire_source;
            struct
            {
                int ping_interval;
                int reconnect_count;
                int reconnect_interval;
                int reconnect_nonce;
            } wss_client_config;
        } feishu;

        struct
        {
            gchar *wss_url;
            int wss_shards;
            gchar *access_token;
            int64_t expires_in;
            GSource *expire_source;
            GSList *conns;
        } qq;

        struct
        {
            struct termios orig_term;
            pthread_t input_tid;
            GThread *input_thread;
            GAsyncQueue *input_queue;
            GSource *queue_source;
            gchar *pending_output;
            pthread_mutex_t term_mutex;
            volatile gboolean running;
            GString *history_path;
        } cli;
    };
} edc_channel_actor_state_t;

extern edc_actor_t *edc_channel_actor_new(struct channel_conf *config, edc_network_async_t *network);
extern void edc_channel_actor_free(edc_actor_t *actor);
extern void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer);
extern void edc_channel_actor_add(edc_actor_t *actor, edc_channel_type type, edc_actor_handler_t handler);

extern struct edc_channel *edc_channel_qq_new(gchar *message_id, gchar *author_id);
extern struct edc_channel *edc_channel_feishu_new(gchar *chat_type, gchar *chat_id, gchar *message_id,
                                                gchar *user_open_id);
extern struct edc_channel *edc_channel_telegram_new(gchar *chat_id);
extern struct edc_channel *edc_channel_cli_new(void);
extern struct edc_channel *edc_channel_copy(struct edc_channel *channel);
extern cJSON *edc_channel_to_json(struct edc_channel *channel);
extern struct edc_channel *edc_channel_from_json(cJSON *channel);
extern void edc_channel_free(struct edc_channel *channel);

extern void edc_channel_message_free(struct edc_channel_message *msg);
extern struct edc_channel_message *edc_channel_message_new(struct edc_channel *channel, gchar *content);
extern struct edc_channel_message *edc_channel_ref_message_new(struct edc_channel *channel, gchar *content);
extern struct edc_channel_message *edc_channel_message_move(struct edc_channel_message *rhs);
extern struct edc_channel_message *edc_channel_message_copy(struct edc_channel_message *rhs, gchar *content);
extern struct edc_channel_message *edc_channel_telegram_message_new(gchar *chat_id, gchar *content);
extern struct edc_channel_message *edc_channel_feishu_message_new(gchar *chat_type, gchar *chat_id, gchar *message_id,
                                                                gchar *user_open_id, gchar *content);
extern struct edc_channel_message *edc_channel_qq_message_new(gchar *message_id, gchar *author_id, gchar *content);

#endif
