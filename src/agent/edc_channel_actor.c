#include "edc_channel_actor.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "sds.h"
#include "uthash.h"
#include "utlist_fix.h"
#include <stdlib.h>

/* ── 边界翻译: channel 消息 → core 的入站输入 ─────────────────
 *
 * `struct edc_sched_input` 是 core 的词表 (edc_scheduler.h); 本文件是"两个词表都
 * 认识"的地方 —— 于是 core 不必知道 channel 消息的形状, 也不用 include 本头文件。
 * 输入**接管消息所有权**: 释放输入时顺带释放消息 (`origin` 只是透传, 但它的
 * 生命周期由这里管 —— 否则要么泄漏, 要么调度器得认识它的类型)。 */
struct edc_sched_input *edc_channel_input_new(struct edc_channel_message *msg)
{
    if (!msg)
        return NULL;
    struct edc_sched_input *input = calloc(1, sizeof(*input));
    if (!input)
    { /* OOM: 这条输入丢弃 (scheduler 对 NULL 负载直接返回), 不泄漏消息 */
        edc_channel_message_free(msg);
        return NULL;
    }
    input->session_id = msg->session_id;
    input->content = msg->content;
    input->type = msg->type;
    input->origin = msg;
    return input;
}

void edc_channel_input_free(struct edc_sched_input *input)
{
    if (!input)
        return;
    struct edc_channel_message *msg = input->origin;
    free(input);
    if (msg)
        edc_channel_message_free(msg);
}

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

cJSON *edc_channel_type_to_json(edc_channel_type type)
{
    cJSON *data = cJSON_CreateObject();
    const char *name = "adapter"; /* 唯一通道实现 */
    (void)type;
    cJSON_AddStringToObject(data, "type", name);
    return data;
}

edc_channel_type edc_channel_type_from_json(cJSON *data)
{
    (void)data;
    return edc_channel_adapter;
}

void edc_channel_free(struct edc_channel *channel)
{
    if (!channel)
        return;

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
    if (msg->task_id)
        sdsfree(msg->task_id);
    if (msg->session_id)
        sdsfree(msg->session_id);
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
