#ifndef __EDC_CHANNEL_ADAPTER_H__
#define __EDC_CHANNEL_ADAPTER_H__

#ifdef __cplusplus
extern "C" {
#endif
#include "edc_actor.h"
extern void edc_channel_adapter_init(edc_actor_t *channel_actor, void *instance,
                                     void (*on_actor_message_send_fn)(void *, edc_actor_msg_t *));
#ifdef __cplusplus
}
#endif

#endif
