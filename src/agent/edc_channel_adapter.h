#ifndef __EDC_CHANNEL_ADAPTER_H__
#define __EDC_CHANNEL_ADAPTER_H__

#ifdef __cplusplus
extern "C" {
#endif
struct edc_actor;
struct edc_channel_message;
extern void edc_channel_adapter_init(struct edc_actor *channel_actor, void *instance,
                                     void (*on_actor_message_send_fn)(void *, struct edc_channel_message *));
#ifdef __cplusplus
}
#endif

#endif
