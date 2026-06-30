#include "edc_string.h"
#ifndef __BC_CHANNEL_ACTOR_H__
#define __BC_CHANNEL_ACTOR_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_network_async.h"
#include "cJSON.h"
#include "uthash.h"
#include <pthread.h>
#include <termios.h>
struct edc_channel
{
    edc_channel_type type;
    union {
        struct
        {
            char *title;
            char *message_id;
            char *reply_to;
            char *refs;
        } email;
        struct
        {
            char *chat_id;
        } telegram;

        struct
        {
            char *chat_type;
            char *chat_id;
            char *message_id;
            char *user_open_id;
        } feishu;

        struct
        {
            char *message_id;
            char *author_id;
            int64_t seq_id;
        } qq;
    };
};

struct edc_channel_message
{
    char *content;
    bool channel_ref;
    struct edc_channel *channel;
};

struct edc_mg_conn_node
{
    struct mg_connection *conn;
    struct edc_mg_conn_node *next;
};

struct edc_handler_entry
{
    int type;
    edc_actor_handler_t handler;
    UT_hash_handle hh;
};

typedef struct
{
    struct channel_conf config;
    edc_network_async_t *network;
    edc_actor_t *observer;
    struct edc_handler_entry *handlers;
    union {
        struct
        {
            bool polling;
            int64_t update_id;
            GSource *poll_source;
        } telegram;

        struct
        {
            char *tenant_access_token;
            int64_t expire;
            char *wss_url;
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
            char *wss_url;
            int wss_shards;
            char *access_token;
            int64_t expires_in;
            GSource *expire_source;
            struct edc_mg_conn_node *conns;
        } qq;

        struct
        {
            struct termios orig_term;
            pthread_t input_tid;
            GThread *input_thread;
            GAsyncQueue *input_queue;
            GSource *queue_source;
            sds pending_output;
            pthread_mutex_t term_mutex;
            volatile int running;
            sds history_path;
        } cli;
    };
} edc_channel_actor_state_t;

extern edc_actor_t *edc_channel_actor_new(struct channel_conf *config, edc_network_async_t *network);
extern void edc_channel_actor_free(edc_actor_t *actor);
extern void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer);
extern void edc_channel_actor_add(edc_actor_t *actor, edc_channel_type type, edc_actor_handler_t handler);

extern struct edc_channel *edc_channel_qq_new(char *message_id, char *author_id);
extern struct edc_channel *edc_channel_feishu_new(char *chat_type, char *chat_id, char *message_id,
                                                char *user_open_id);
extern struct edc_channel *edc_channel_telegram_new(char *chat_id);
extern struct edc_channel *edc_channel_cli_new(void);
extern struct edc_channel *edc_channel_copy(struct edc_channel *channel);
extern cJSON *edc_channel_to_json(struct edc_channel *channel);
extern struct edc_channel *edc_channel_from_json(cJSON *channel);
extern void edc_channel_free(struct edc_channel *channel);

extern void edc_channel_message_free(struct edc_channel_message *msg);
extern struct edc_channel_message *edc_channel_message_new(struct edc_channel *channel, char *content);
extern struct edc_channel_message *edc_channel_ref_message_new(struct edc_channel *channel, char *content);
extern struct edc_channel_message *edc_channel_message_move(struct edc_channel_message *rhs);
extern struct edc_channel_message *edc_channel_message_copy(struct edc_channel_message *rhs, char *content);
extern struct edc_channel_message *edc_channel_telegram_message_new(char *chat_id, char *content);
extern struct edc_channel_message *edc_channel_feishu_message_new(char *chat_type, char *chat_id, char *message_id,
                                                                char *user_open_id, char *content);
extern struct edc_channel_message *edc_channel_qq_message_new(char *message_id, char *author_id, char *content);

#endif
