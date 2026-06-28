#include "sds.h"
#include "edc_commands.h"
#include "edc_actor.h"
#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_memory_actor.h"
#include <edc_session.h>
#include <log.h>
#include <signal.h>

extern volatile sig_atomic_t _run_loop;

static bool edc_commands_is_target(const char *content, const char *target)
{
    sds cmd = sdsnew(content);
    cmd = sdstrim(cmd, " \t\r\n");
    bool is = strcmp(cmd, target) == 0;
    sdsfree(cmd);

    return is;
}

static bool edc_commands_handle_new(struct edc_agent *agent, edc_actor_t *actor, struct edc_channel *channel)
{

    edc_session_reload(agent->session, FALSE);
    edc_agent_remove_all_turns(agent);

    if (!edc_memory_actor_is_compressing(agent->memory_actor)) // msg + system prompt
    {
        edc_actor_send(agent->memory_actor,
                      edc_actor_msg_new(BC_ACTOR_MSG_MEMORY_COMPRESS, agent->scheduler, NULL,
                                        (void *)(intptr_t)1 /* force */, NULL));
    }

    struct edc_channel_message *reply =
        edc_channel_message_new(edc_channel_copy(channel), sdsnew("[system] start new session now."));
    edc_actor_send(agent->channel_actor,
                  edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->channel->type, actor, NULL, reply,
                                            (edc_actor_data_free_fn)edc_channel_message_free));
    log_info("receive /new command. save session and remove all turns");
    return TRUE;
}

static bool edc_commands_handle_stop(struct edc_agent *agent, edc_actor_t *actor, struct edc_channel *channel)
{
    edc_session_reload(agent->session, TRUE);
    edc_agent_remove_all_turns(agent);

    struct edc_channel_message *reply =
        edc_channel_message_new(edc_channel_copy(channel), sdsnew("[system] stop all turn."));
    edc_actor_send(agent->channel_actor,
                  edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->channel->type, actor, NULL, reply,
                                            (edc_actor_data_free_fn)edc_channel_message_free));

    return TRUE;
}

static bool edc_commands_handle_help(struct edc_agent *agent, edc_actor_t *actor, struct edc_channel *channel)
{
    const char *help_text =
        "# EdgeClaw Commands\n\n"
        "- `/new` — Start a new session (clear current turns, trigger memory compress)\n"
        "- `/stop` — Stop all current turns and save session history\n"
        "- `/help` — Show this help message\n"
        "- `/exit` — Exit the program gracefully";

    struct edc_channel_message *reply =
        edc_channel_message_new(edc_channel_copy(channel), sdsnew(help_text));
    edc_actor_send(agent->channel_actor,
                  edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->channel->type, actor, NULL, reply,
                                            (edc_actor_data_free_fn)edc_channel_message_free));

    return TRUE;
}

static bool edc_commands_handle_exit(struct edc_agent *agent, edc_actor_t *actor, struct edc_channel *channel)
{
    struct edc_channel_message *reply =
        edc_channel_message_new(edc_channel_copy(channel), sdsnew("[system] shutting down..."));
    edc_actor_send(agent->channel_actor,
                  edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, reply->channel->type, actor, NULL, reply,
                                            (edc_actor_data_free_fn)edc_channel_message_free));

    log_info("received /exit command");
    _run_loop = 0;
    return TRUE;
}

bool edc_commands_handler(struct edc_agent *agent, struct edc_actor *actor, struct edc_channel_message *message)
{
    if (!message || !message->channel || !message->content)
    {
        return FALSE;
    }

    if (edc_commands_is_target(message->content, "/new"))
    {
        return edc_commands_handle_new(agent, actor, message->channel);
    }
    else if (edc_commands_is_target(message->content, "/stop"))
    {
        return edc_commands_handle_stop(agent, actor, message->channel);
    }
    else if (edc_commands_is_target(message->content, "/help"))
    {
        return edc_commands_handle_help(agent, actor, message->channel);
    }
    else if (edc_commands_is_target(message->content, "/exit"))
    {
        return edc_commands_handle_exit(agent, actor, message->channel);
    }
    else
    {
        return FALSE;
    }
}
