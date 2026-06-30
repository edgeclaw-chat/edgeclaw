#include "edc_string.h"
#include "edc_strutil.h"
#include "edc_channel_actor.h"
#include "edc_actor.h"
#include "edc_channel_cli.h"
#include "uthash.h"
#include "utlist.h"
#include "edc_channel_feishu.h"
#include "edc_channel_qq.h"
#include "edc_channel_telegram.h"
#include "edc_config.h"
#include "edc_network_async.h"
#include <stdlib.h>

static void edc_channel_actor_handle(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;

    if (msg->type == BC_ACTOR_MSG_START)
    {
        struct edc_handler_entry *e, *tmp;
        HASH_ITER(hh, state->handlers, e, tmp) {
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

struct edc_channel *edc_channel_feishu_new(char *chat_type, char *chat_id, char *message_id, char *user_open_id)
{
    struct edc_channel *channel = g_new0(struct edc_channel, 1);
    channel->type = edc_channel_feishu;
    channel->feishu.chat_type = chat_type;
    channel->feishu.chat_id = chat_id;
    channel->feishu.message_id = message_id;
    channel->feishu.user_open_id = user_open_id;
    return channel;
}

struct edc_channel_message *edc_channel_feishu_message_new(char *chat_type, char *chat_id, char *message_id,
                                                         char *user_open_id, char *content)
{
    struct edc_channel_message *msg = g_new0(struct edc_channel_message, 1);
    msg->channel = edc_channel_feishu_new(chat_type, chat_id, message_id, user_open_id);
    msg->content = content;
    return msg;
}

struct edc_channel *edc_channel_cli_new(void)
{
    struct edc_channel *channel = g_new0(struct edc_channel, 1);
    channel->type = edc_channel_cli;
    return channel;
}

struct edc_channel *edc_channel_telegram_new(char *chat_id)
{
    struct edc_channel *channel = g_new0(struct edc_channel, 1);
    channel->type = edc_channel_telegram;
    channel->telegram.chat_id = chat_id;
    return channel;
}

cJSON *edc_channel_to_json(struct edc_channel *channel)
{
    cJSON *data = cJSON_CreateObject();
    if (channel->type == edc_channel_qq)
    {
        cJSON_AddStringToObject(data, "type", "edc_channel_qq");
        cJSON_AddStringToObject(data, "author_id", channel->qq.author_id);
        cJSON_AddStringToObject(data, "message_id", channel->qq.message_id);
    }
    else if (channel->type == edc_channel_feishu)
    {
        cJSON_AddStringToObject(data, "type", "edc_channel_feishu");
        cJSON_AddStringToObject(data, "chat_type", channel->feishu.chat_type);
        cJSON_AddStringToObject(data, "chat_id", channel->feishu.chat_id);
        cJSON_AddStringToObject(data, "message_id", channel->feishu.message_id);
        cJSON_AddStringToObject(data, "user_open_id", channel->feishu.user_open_id);
    }
    else if (channel->type == edc_channel_telegram)
    {
        cJSON_AddStringToObject(data, "type", "edc_channel_telegram");
        cJSON_AddStringToObject(data, "chat_id", channel->telegram.chat_id);
    }
    else if (channel->type == edc_channel_cli)
    {
        cJSON_AddStringToObject(data, "type", "edc_channel_cli");
    }

    return data;
}

struct edc_channel *edc_channel_from_json(cJSON *data)
{
    cJSON *type = cJSON_GetObjectItem(data, "type");
    if (!type || !cJSON_IsString(type))
    {
        return NULL;
    }

    cJSON *author_id = cJSON_GetObjectItem(data, "author_id");
    cJSON *message_id = cJSON_GetObjectItem(data, "message_id");
    cJSON *chat_type = cJSON_GetObjectItem(data, "chat_type");
    cJSON *chat_id = cJSON_GetObjectItem(data, "chat_id");
    cJSON *user_open_id = cJSON_GetObjectItem(data, "user_open_id");

    if (edc_str_equal(cJSON_GetStringValue(type), "edc_channel_qq"))
    {
        if (author_id && message_id && cJSON_IsString(author_id) && cJSON_IsString(message_id))
        {
            struct edc_channel *qchannel = edc_channel_qq_new(strdup(cJSON_GetStringValue(message_id)),
                                                            strdup(cJSON_GetStringValue(author_id)));
            return qchannel;
        }
    }
    else if (edc_str_equal(cJSON_GetStringValue(type), "edc_channel_feishu"))
    {
        if (chat_type && chat_id && message_id && user_open_id && cJSON_IsString(chat_type) &&
            cJSON_IsString(chat_id) && cJSON_IsString(message_id) && cJSON_IsString(user_open_id))
        {
            struct edc_channel *feishu_channel = edc_channel_feishu_new(
                strdup(cJSON_GetStringValue(chat_type)), strdup(cJSON_GetStringValue(chat_id)),
                strdup(cJSON_GetStringValue(message_id)), strdup(cJSON_GetStringValue(user_open_id)));

            return feishu_channel;
        }
    }
    else if (edc_str_equal(cJSON_GetStringValue(type), "edc_channel_telegram"))
    {
        if (chat_id && cJSON_IsString(chat_id))
        {
            return edc_channel_telegram_new(strdup(cJSON_GetStringValue(chat_id)));
        }
    }
    else if (edc_str_equal(cJSON_GetStringValue(type), "edc_channel_cli"))
    {
        return edc_channel_cli_new();
    }

    return NULL;
}

struct edc_channel_message *edc_channel_telegram_message_new(char *chat_id, char *content)
{
    struct edc_channel_message *msg = g_new0(struct edc_channel_message, 1);
    msg->channel = edc_channel_telegram_new(chat_id);
    msg->content = content;
    return msg;
}

struct edc_channel *edc_channel_qq_new(char *message_id, char *author_id)
{
    struct edc_channel *channel = g_new0(struct edc_channel, 1);
    channel->type = edc_channel_qq;
    channel->qq.author_id = author_id;
    channel->qq.message_id = message_id;
    channel->qq.seq_id = rand();
    return channel;
}

struct edc_channel_message *edc_channel_qq_message_new(char *message_id, char *author_id, char *content)
{
    struct edc_channel_message *msg = g_new0(struct edc_channel_message, 1);
    msg->channel = edc_channel_qq_new(message_id, author_id);
    msg->content = content;
    return msg;
}

void edc_channel_free(struct edc_channel *channel)
{
    if (!channel)
        return;

    if (channel->type == edc_channel_qq)
    {
        if (channel->qq.author_id)
        {
            free(channel->qq.author_id);
        }

        if (channel->qq.message_id)
        {
            free(channel->qq.message_id);
        }
    }
    else if (channel->type == edc_channel_feishu)
    {
        if (channel->feishu.chat_type)
            free(channel->feishu.chat_type);
        if (channel->feishu.chat_id)
            free(channel->feishu.chat_id);
        if (channel->feishu.message_id)
            free(channel->feishu.message_id);
        if (channel->feishu.user_open_id)
            free(channel->feishu.user_open_id);
    }
    else if (channel->type == edc_channel_telegram)
    {
        free(channel->telegram.chat_id);
    }
    else if (channel->type == edc_channel_cli)
    {
    }

    free(channel);
}

struct edc_channel *edc_channel_copy(struct edc_channel *rhs)
{
    if (!rhs)
        return NULL;

    if (rhs->type == edc_channel_telegram)
    {
        return edc_channel_telegram_new(strdup(rhs->telegram.chat_id));
    }
    else if (rhs->type == edc_channel_cli)
    {
        return edc_channel_cli_new();
    }
    else if (rhs->type == edc_channel_feishu)
    {
        return edc_channel_feishu_new(strdup(rhs->feishu.chat_type), strdup(rhs->feishu.chat_id),
                                     strdup(rhs->feishu.message_id), strdup(rhs->feishu.user_open_id));
    }
    else if (rhs->type == edc_channel_qq)
    {
        return edc_channel_qq_new(strdup(rhs->qq.message_id), strdup(rhs->qq.author_id));
    }

    return NULL;
}

#define __MOVE(lhs, rhs, member)                                                                                       \
    ((lhs)->member) = ((rhs)->member);                                                                                 \
    ((rhs)->member) = NULL
struct edc_channel *edc_channel_move(struct edc_channel *rhs)
{
    struct edc_channel *lhs = g_new0(struct edc_channel, 1);
    lhs->type = rhs->type;

    if (rhs->type == edc_channel_telegram)
    {
        __MOVE(lhs, rhs, telegram.chat_id);
    }
    else if (rhs->type == edc_channel_cli)
    {
    }
    else if (rhs->type == edc_channel_feishu)
    {
        __MOVE(lhs, rhs, feishu.chat_type);
        __MOVE(lhs, rhs, feishu.chat_id);
        __MOVE(lhs, rhs, feishu.message_id);
        __MOVE(lhs, rhs, feishu.user_open_id);
    }
    else if (rhs->type == edc_channel_qq)
    {
        __MOVE(lhs, rhs, qq.author_id);
        __MOVE(lhs, rhs, qq.message_id);
        lhs->qq.seq_id = rhs->qq.seq_id;
    }

    return lhs;
}

#undef __MOVE

struct edc_channel_message *edc_channel_message_new(struct edc_channel *channel, char *content)
{
    struct edc_channel_message *message = g_new0(struct edc_channel_message, 1);
    message->channel = channel;
    message->content = content;
    message->channel_ref = FALSE;
    if (message->channel->type == edc_channel_qq)
    {
        message->channel->qq.seq_id += 1;
    }
    return message;
}

extern struct edc_channel_message *edc_channel_ref_message_new(struct edc_channel *channel, char *content)
{
    struct edc_channel_message *message = g_new0(struct edc_channel_message, 1);
    message->channel = channel;
    message->content = content;
    message->channel_ref = TRUE;
    if (message->channel->type == edc_channel_qq)
    {
        message->channel->qq.seq_id += 1;
    }

    return message;
}

struct edc_channel_message *edc_channel_message_copy(struct edc_channel_message *rhs, char *content)
{
    if (!content)
    {
        content = strdup(rhs->content);
    }

    if (rhs->channel_ref)
    {
        return edc_channel_ref_message_new(edc_channel_copy(rhs->channel), content);
    }
    else
    {
        return edc_channel_message_new(edc_channel_copy(rhs->channel), content);
    }
}

void edc_channel_message_free(struct edc_channel_message *msg)
{
    if (msg->content)
    {
        free(msg->content);
    }

    if (msg->channel && !msg->channel_ref)
    {
        edc_channel_free(msg->channel);
    }
    free(msg);
}

struct edc_channel_message *edc_channel_message_move(struct edc_channel_message *rhs)
{
    struct edc_channel_message *message = g_new0(struct edc_channel_message, 1);
    message->content = rhs->content;
    rhs->content = NULL;
    if (rhs->channel_ref)
    {
        message->channel = rhs->channel;
        rhs->channel = NULL;
    }
    else
    {
        message->channel = edc_channel_move(rhs->channel);
    }

    message->channel_ref = rhs->channel_ref;
    return message;
}

edc_actor_t *edc_channel_actor_new(struct channel_conf *config, edc_network_async_t *network)
{
    edc_channel_actor_state_t *state = g_new0(edc_channel_actor_state_t, 1);
    state->config = *config;

    state->network = network;
    state->observer = NULL;

    edc_actor_t *actor = edc_actor_new("edc_channel_actor", edc_channel_actor_handle, state);

    if (config->type == edc_channel_telegram)
    {
        edc_channel_telegram_init(actor);
    }
    else if (config->type == edc_channel_feishu)
    {
        edc_channel_feishu_init(actor);
    }
    else if (config->type == edc_channel_qq)
    {
        edc_channel_qq_init(actor);
    }
    else if (config->type == edc_channel_cli)
    {
        edc_channel_cli_init(actor);
    }
    return actor;
}

void edc_channel_actor_free(edc_actor_t *actor)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;
    {
        struct edc_handler_entry *e, *tmp;
        HASH_ITER(hh, state->handlers, e, tmp) {
            HASH_DEL(state->handlers, e);
            free(e);
        }
    }

    switch (state->config.type)
    {
    case edc_channel_telegram:
        if (state->telegram.poll_source)
            g_source_unref(state->telegram.poll_source);
        break;
    case edc_channel_feishu:
        if (state->feishu.wss_url)
        {
            free(state->feishu.wss_url);
        }

        if (state->feishu.tenant_access_token)
        {
            free(state->feishu.tenant_access_token);
        }

        if (state->feishu.expire_source)
        {
            g_source_unref(state->feishu.expire_source);
        }
        break;

    case edc_channel_qq:
        if (state->qq.access_token)
        {
            free(state->qq.access_token);
        }

        if (state->qq.expire_source)
        {
            g_source_unref(state->qq.expire_source);
        }

        if (state->qq.wss_url)
        {
            free(state->qq.wss_url);
        }

        if (state->qq.conns)
        {
            struct edc_mg_conn_node *node, *tmp;
            LL_FOREACH_SAFE(state->qq.conns, node, tmp)
            {
                LL_DELETE(state->qq.conns, node);
                free(node);
            }
        }
        break;

    case edc_channel_cli:
        g_atomic_int_set(&state->cli.running, FALSE);
        if (state->cli.input_thread)
        {
            pthread_kill(state->cli.input_tid, SIGUSR1);
            g_thread_join(state->cli.input_thread);
        }
        if (state->cli.input_queue)
        {
            char *line;
            while ((line = g_async_queue_try_pop(state->cli.input_queue)))
            {
                free(line);
            }
            g_async_queue_unref(state->cli.input_queue);
        }
        if (state->cli.queue_source)
        {
            g_source_destroy(state->cli.queue_source);
            g_source_unref(state->cli.queue_source);
        }
        if (state->cli.pending_output)
            edc_str_free(state->cli.pending_output);
        if (state->cli.history_path)
            edc_str_free(state->cli.history_path);
        pthread_mutex_destroy(&state->cli.term_mutex);
        tcsetattr(STDIN_FILENO, TCSANOW, &state->cli.orig_term);
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
