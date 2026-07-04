#include "sds.h"
#ifndef __BC_CHANNEL_ACTOR_H__
#define __BC_CHANNEL_ACTOR_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_containers.h"
#include "edc_terminal.h"
#include "cJSON.h"
#include "uthash.h"
#include <uv.h>
struct edc_channel
{
    edc_channel_type type;
    union {
        struct
        {
            sds title;
            sds message_id;
            sds reply_to;
            sds refs;
        } email;
    };
};

struct edc_channel_message
{
    sds content;
    bool channel_ref;
    struct edc_channel *channel;
};

struct edc_handler_entry
{
    int type;
    edc_actor_handler_t handler;
    UT_hash_handle hh;
};

typedef struct
{
    edc_actor_t *actor;
    struct channel_conf config;
    edc_actor_t *observer;
    struct edc_handler_entry *handlers;
    union {
        struct
        {
            struct edc_terminal_state orig_term;
            uv_tty_t stdin_tty;
            sds line_buffer;
            int cursor_pos;
            sds pending_output;
            sds *history;
            int history_count;
            int history_capacity;
            int history_pos;
            sds saved_line;
            sds history_path;
            volatile int running;
            bool tty_stopped;
        } cli;
    };
} edc_channel_actor_state_t;

extern edc_actor_t *edc_channel_actor_new(struct channel_conf *config, const char *workspace);
extern void edc_channel_actor_free(edc_actor_t *actor);
extern void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer);
extern void edc_channel_actor_add(edc_actor_t *actor, edc_channel_type type, edc_actor_handler_t handler);

extern struct edc_channel *edc_channel_cli_new(void);
extern struct edc_channel *edc_channel_copy(struct edc_channel *channel);
extern cJSON *edc_channel_to_json(struct edc_channel *channel);
extern struct edc_channel *edc_channel_from_json(cJSON *channel);
extern void edc_channel_free(struct edc_channel *channel);

extern void edc_channel_message_free(struct edc_channel_message *msg);
extern struct edc_channel_message *edc_channel_message_new(struct edc_channel *channel, sds content);
extern struct edc_channel_message *edc_channel_ref_message_new(struct edc_channel *channel, sds content);
extern struct edc_channel_message *edc_channel_message_move(struct edc_channel_message *rhs);
extern struct edc_channel_message *edc_channel_message_copy(struct edc_channel_message *rhs, sds content);

#endif
