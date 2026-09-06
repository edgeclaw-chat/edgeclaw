#ifndef __BC_COMMANDS_H__
#define __BC_COMMANDS_H__

#include "edc_config.h"
#include <stdbool.h>
struct edc_actor;
struct edc_agent;
struct edc_channel_message;
extern bool edc_commands_handler(struct edc_agent *, struct edc_actor *actor, struct edc_channel_message *);
extern void edc_commands_reply(struct edc_agent *agent, struct edc_actor *actor, edc_channel_type channel_type,
                               const char *text);

#endif
