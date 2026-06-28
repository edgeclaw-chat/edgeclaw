#ifndef __BC_ACTOR_H__
#define __BC_ACTOR_H__

#include <glib.h>
#include "sds.h"
#include <stdbool.h>

typedef struct edc_actor edc_actor_t;
typedef struct edc_actor_msg edc_actor_msg_t;

typedef enum
{
    BC_ACTOR_MSG_START = 0,
    BC_ACTOR_MSG_USER_INPUT,
    BC_ACTOR_MSG_LLM_REQUEST,
    BC_ACTOR_MSG_LLM_RESPONSE,
    BC_ACTOR_MSG_TOOL_REQUEST,
    BC_ACTOR_MSG_TOOL_RESPONSE,
    BC_ACTOR_MSG_CHANNEL_SEND,
    BC_ACTOR_MSG_CHANNEL_RECV,
    BC_ACTOR_MSG_MEMORY_COMPRESS,
    BC_ACTOR_MSG_MEMORY_DONE,
} edc_actor_msg_type_t;

extern const char *edc_actor_msg_type_s(edc_actor_msg_type_t type);
typedef void (*edc_actor_data_free_fn)(void *);

struct edc_actor_msg
{
    edc_actor_msg_type_t type;
    int sub_type;
    edc_actor_t *from;
    void* request_id;
    void *data;
    edc_actor_data_free_fn data_free_fn;
};

typedef void (*edc_actor_handler_t)(edc_actor_t *actor, edc_actor_msg_t *msg);

struct edc_actor
{
    GAsyncQueue *mailbox;
    GMainContext *context;
    edc_actor_handler_t handler;
    void *state;
    sds name;
};

typedef struct
{
    GThread *thread;
    GMainLoop *loop;
    GMainContext *context;
    GArray *actors;
    int running;
} edc_actor_runner_t;

edc_actor_runner_t *edc_actor_runner_new();
void edc_actor_runner_free(edc_actor_runner_t *runner);
void edc_actor_runner_start(edc_actor_runner_t *runner);
void edc_actor_runner_stop(edc_actor_runner_t *runner);
void edc_actor_runner_add(edc_actor_runner_t *runner, edc_actor_t *actor);

edc_actor_t *edc_actor_new(const char *name, edc_actor_handler_t handler, void *state);
void edc_actor_free(edc_actor_t *actor);
void edc_actor_send(edc_actor_t *actor, edc_actor_msg_t *msg);
edc_actor_msg_t *edc_actor_msg_new(edc_actor_msg_type_t type, edc_actor_t *from, void* request_id, void *data,
                                 edc_actor_data_free_fn fn);
edc_actor_msg_t *edc_actor_msg_new_sub_type(edc_actor_msg_type_t type, int sub_type, edc_actor_t *from, void* request_id,
                                          void *data, edc_actor_data_free_fn fn);
void edc_actor_msg_free(edc_actor_msg_t *msg);

#endif
