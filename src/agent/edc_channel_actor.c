#include "edc_channel_actor.h"
#include "edc_actor.h"
#include "edc_channel_cli.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "sds.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <stdlib.h>

static void edc_channel_actor_handle(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;

    if (msg->type == BC_ACTOR_MSG_START)
    {
        struct edc_handler_entry *e, *tmp;
        HASH_ITER(hh, state->handlers, e, tmp)
        {
            e->handler(actor, msg);
        }
    }
    else
    {
        struct edc_handler_entry *e;
        HASH_FIND_INT(state->handlers, &msg->sub_type, e);
        if (e)
            e->handler(actor, msg);
    }

    edc_actor_msg_free(msg);
    return;
}

struct edc_channel *edc_channel_cli_new(void)
{
    struct edc_channel *channel = g_new0(struct edc_channel, 1);
    channel->type = edc_channel_cli;
    return channel;
}

cJSON *edc_channel_type_to_json(edc_channel_type type)
{
    cJSON *data = cJSON_CreateObject();
    if (type == edc_channel_cli)
    {
        cJSON_AddStringToObject(data, "type", "edc_channel_cli");
    }

    return data;
}

edc_channel_type edc_channel_type_from_json(cJSON *data)
{
    cJSON *type = cJSON_GetObjectItem(data, "type");
    if (!type || !cJSON_IsString(type))
    {
        return edc_channel_cli;
    }

    if (strcmp(cJSON_GetStringValue(type), "edc_channel_cli") == 0)
    {
        return edc_channel_cli;
    }

    return edc_channel_cli;
}

void edc_channel_free(struct edc_channel *channel)
{
    if (!channel)
        return;

    if (channel->type == edc_channel_cli)
    {
    }

    free(channel);
}

struct edc_channel_message *edc_channel_message_new(edc_channel_type type, edc_channel_message_type message_type,
                                                    sds content)
{
    struct edc_channel_message *message = g_new0(struct edc_channel_message, 1);
    message->type = type;
    message->message_type = message_type;
    message->content = content;
    return message;
}

void edc_channel_message_free(struct edc_channel_message *msg)
{
    if (!msg)
        return;
    if (msg->header)
        sdsfree(msg->header);
    sdsfree(msg->content);
    free(msg);
}

edc_actor_t *edc_channel_actor_new(struct channel_conf *config, const char *workspace)
{
    edc_channel_actor_state_t *state = calloc(1, sizeof(edc_channel_actor_state_t));
    state->config = *config;

    state->observer = NULL;

    edc_actor_t *actor = edc_actor_new("edc_channel_actor", edc_channel_actor_handle, state);
    state->actor = actor;

    if (config->type == edc_channel_cli)
    {
        edc_channel_cli_init(actor);
    }

    state->adapter.instance = NULL;
    state->adapter.on_actor_message_send_fn = NULL;
    return actor;
}

void edc_channel_actor_free(edc_actor_t *actor)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;
    {
        struct edc_handler_entry *e, *tmp;
        HASH_ITER(hh, state->handlers, e, tmp)
        {
            HASH_DEL(state->handlers, e);
            free(e);
        }
    }

    switch (state->config.type)
    {
    case edc_channel_cli:
        state->cli.running = 0;
        if (state->cli.pending_output)
            sdsfree(state->cli.pending_output);
        if (state->cli.history_path)
            sdsfree(state->cli.history_path);
        if (state->cli.line_buffer)
            sdsfree(state->cli.line_buffer);
        if (state->cli.saved_line)
            sdsfree(state->cli.saved_line);
        for (int i = 0; i < state->cli.history_count; i++)
            sdsfree(state->cli.history[i]);
        free(state->cli.history);
        if (!state->cli.tty_stopped)
        {
            if (!uv_is_closing((uv_handle_t *)&state->cli.stdin_tty))
            {
                uv_read_stop((uv_stream_t *)&state->cli.stdin_tty);
                uv_close((uv_handle_t *)&state->cli.stdin_tty, NULL);
            }
        }
        edc_terminal_restore(STDIN_FILENO, &state->cli.orig_term);
        break;
    default:
        break;
    }

    free(state);
    edc_actor_free(actor);
}

void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;
    state->observer = observer;
}

void edc_channel_actor_add(edc_actor_t *actor, edc_channel_type type, edc_actor_handler_t handler)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;
    struct edc_handler_entry *e = g_new0(struct edc_handler_entry, 1);
    e->type = (int)type;
    e->handler = handler;
    HASH_ADD_INT(state->handlers, type, e);
}
