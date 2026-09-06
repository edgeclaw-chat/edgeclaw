#include "edc_channel_adapter.h"
#include "edc_channel_actor.h"

static void edc_channel_adapter_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;
    switch (msg->type)
    {
    case BC_ACTOR_MSG_START:
        break;
    case BC_ACTOR_MSG_CHANNEL_SEND:
        if (state->adapter.on_actor_message_send_fn)
        {
            state->adapter.on_actor_message_send_fn(state->adapter.instance, msg);
        }
        break;
    default:
        break;
    }
}

void edc_channel_adapter_init(edc_actor_t *channel_actor, void *instance,
                              void (*on_actor_message_send_fn)(void *, edc_actor_msg_t *))
{
    edc_channel_actor_state_t *state = channel_actor->state;
    state->adapter.instance = instance;
    state->adapter.on_actor_message_send_fn = on_actor_message_send_fn;

    edc_channel_actor_add(channel_actor, edc_channel_adapter, edc_channel_adapter_handler);
}

