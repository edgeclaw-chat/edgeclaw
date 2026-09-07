#include "edc_c_bridge.h"
#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "uv.h"
#include <edc_actor.h>
#include <edc_file_utils.h>

/*
 * Bridge implementation. Compiled as C (this is a .c file), so it can include
 * edc_agent.h and the sds/cJSON headers it pulls in. The TUI (C++) only sees
 * edc_c_bridge.h, which uses plain C types.
 */

struct edc_agent *edc_c_agent_new(bool setup, bool enable_sandbox)
{
    return edc_agent_new(setup, enable_sandbox);
}

void edc_c_agent_free(struct edc_agent *agent)
{
    edc_agent_free(agent);
}

struct edc_actor *edc_c_agent_channel_actor(struct edc_agent *agent)
{
    return agent ? agent->channel_actor : NULL;
}

const char *edc_c_channel_message_content(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->content : NULL;
}

int edc_c_channel_message_is_user(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_USER_TYPE : 0;
}

int edc_c_channel_message_is_tool(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_TOOL_TYPE : 0;
}

int edc_c_channel_message_is_thought(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_THOUGHT_TYPE : 0;
}

int edc_c_channel_message_is_resp(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_RESP_TYPE : 0;
}

int edc_c_channel_message_is_sys(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_SYS_TYPE : 0;
}

void edc_c_file_utils_init(struct uv_loop_s *loop)
{
    edc_file_utils_init(loop);
}

void edc_c_agent_send_user_message(struct edc_agent *agent, const char *content)
{
    if (!agent)
        return;

    struct edc_channel_message *message =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));

    edc_channel_actor_state_t *state = agent->channel_actor->state;
    edc_actor_msg_t *actor_msg = edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, agent->channel_actor, NULL, message,
                                                   (edc_actor_data_free_fn)edc_channel_message_free);
    edc_actor_send(state->observer, actor_msg);
}
