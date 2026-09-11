#ifndef __BC_CHANNEL_ACTOR_H__
#define __BC_CHANNEL_ACTOR_H__

#include "cJSON.h"
#include "edc_actor.h"
#include "edc_config.h"
#include "edc_containers.h"
#include "edc_terminal.h"
#include "uthash.h"
#include <uv.h>
struct edc_channel
{
    edc_channel_type type;
};

struct edc_channel_message
{
    sds content;
    sds header;  /* optional short description (e.g. executed command/tool name) shown above content */
    sds task_id; /* id of the task that produced this message (NULL when the
                  * sender is not a task, e.g. user input or a snapshot).
                  * Rendering ends use it to label which task a chat message
                  * belongs to ("task 1"), so interleaved tasks stay readable. */
    edc_channel_type type;
    edc_channel_message_type message_type;
    edc_render_pref_t render_pref; /* 发送方声明的渲染偏好；零值 PLAIN，未声明即纯文本 */
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
    struct
    {
        void *instance;
        void (*on_actor_message_send_fn)(void *instance, struct edc_channel_message *msg);
    } adapter;
} edc_channel_actor_state_t;

extern edc_actor_t *edc_channel_actor_new(struct channel_conf *config, const char *workspace);
extern void edc_channel_actor_free(edc_actor_t *actor);
extern void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer);
extern void edc_channel_actor_add(edc_actor_t *actor, edc_channel_type type, edc_actor_handler_t handler);

extern cJSON *edc_channel_type_to_json(edc_channel_type type);
extern edc_channel_type edc_channel_type_from_json(cJSON *channel);
extern void edc_channel_free(struct edc_channel *channel);

extern void edc_channel_message_free(struct edc_channel_message *msg);
extern struct edc_channel_message *edc_channel_message_new(edc_channel_type type, edc_channel_message_type message_type,
                                                           sds content);

#endif
