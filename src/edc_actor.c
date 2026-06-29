#include "edc_actor.h"

const char *edc_actor_msg_type_s(edc_actor_msg_type_t type)
{
    static const char *__msg_type_s[] = {
        "BC_ACTOR_MSG_START",        "BC_ACTOR_MSG_USER_INPUT",   "BC_ACTOR_MSG_LLM_REQUEST",
        "BC_ACTOR_MSG_LLM_RESPONSE", "BC_ACTOR_MSG_TOOL_REQUEST", "BC_ACTOR_MSG_TOOL_RESPONSE",
        "BC_ACTOR_MSG_CHANNEL_SEND", "BC_ACTOR_MSG_CHANNEL_RECV", "BC_ACTOR_MSG_MEMORY_COMPRESS",
        "BC_ACTOR_MSG_MEMORY_DONE",
    };

    return __msg_type_s[type];
}

edc_actor_runner_t *edc_actor_runner_new()
{
    edc_actor_runner_t *runner = g_new0(edc_actor_runner_t, 1);
    runner->context = g_main_context_new();
    runner->loop = g_main_loop_new(runner->context, FALSE);
    runner->running = FALSE;
    runner->thread = NULL;
    runner->actors = g_array_new(FALSE, FALSE, sizeof(edc_actor_t *));
    return runner;
}

static void* actor_thread_func(void* data)
{
    edc_actor_runner_t *runner = data;

    runner->running = TRUE;
    g_main_context_push_thread_default(runner->context);
    g_main_loop_run(runner->loop);
    g_main_context_pop_thread_default(runner->context);
    runner->running = FALSE;

    return NULL;
}

void edc_actor_runner_start(edc_actor_runner_t *runner)
{
    if (!runner)
        return;
    runner->thread = g_thread_new("edc_actor_runner", actor_thread_func, runner);

    for (int i = 0; i < runner->actors->len; ++i)
    {
        edc_actor_t *actor = g_array_index(runner->actors, edc_actor_t *, i);
        edc_actor_send(actor, edc_actor_msg_new(BC_ACTOR_MSG_START, NULL, NULL, NULL, NULL));
    }
}

void edc_actor_runner_stop(edc_actor_runner_t *runner)
{
    if (!runner || !runner->running)
        return;

    g_main_loop_quit(runner->loop);
    g_thread_join(runner->thread);
}

void edc_actor_runner_free(edc_actor_runner_t *runner)
{
    if (!runner)
        return;

    if (runner->running)
    {
        edc_actor_runner_stop(runner);
    }

    g_main_loop_unref(runner->loop);
    g_main_context_unref(runner->context);
    g_array_free(runner->actors, TRUE);
    free(runner);
}

void edc_actor_runner_add(edc_actor_runner_t *runner, edc_actor_t *actor)
{
    actor->context = runner->context;
    g_array_append_val(runner->actors, actor);
}

edc_actor_msg_t *edc_actor_msg_new_sub_type(edc_actor_msg_type_t type, int sub_type, edc_actor_t *from, void* request_id,
                                          void *data, edc_actor_data_free_fn fn)
{
    edc_actor_msg_t *msg = g_new0(edc_actor_msg_t, 1);
    msg->type = type;
    msg->sub_type = sub_type;
    msg->data = data;
    msg->from = from;
    if (request_id)
    {
        msg->request_id = request_id;
    }
    else
    {
        char *uuid = g_uuid_string_random();
        msg->request_id = request_id ? request_id : (void *)(int64_t)(g_str_hash(uuid));
        free(uuid);
    }
    msg->data_free_fn = fn;

    return msg;
}

edc_actor_msg_t *edc_actor_msg_new(edc_actor_msg_type_t type, edc_actor_t *from, void* request_id, void *data,
                                 edc_actor_data_free_fn fn)
{
    return edc_actor_msg_new_sub_type(type, 0, from, request_id, data, fn);
}

void edc_actor_msg_free(edc_actor_msg_t *msg)
{
    if (!msg)
        return;

    if (msg->data_free_fn && msg->data)
    {
        msg->data_free_fn(msg->data);
    }
    free(msg);
}

edc_actor_t *edc_actor_new(const char *name, edc_actor_handler_t handler, void *state)
{
    edc_actor_t *actor = g_new0(edc_actor_t, 1);
    actor->name = g_strdup(name);
    actor->handler = handler;
    actor->state = state;
    actor->mailbox = g_async_queue_new();
    actor->context = NULL;
    return actor;
}

void edc_actor_free(edc_actor_t *actor)
{
    if (!actor)
        return;

    if (actor->mailbox)
    {
        edc_actor_msg_t *msg;
        while ((msg = g_async_queue_try_pop(actor->mailbox)) != NULL)
        {
            edc_actor_msg_free(msg);
        }
        g_async_queue_unref(actor->mailbox);
    }

    if (actor->name)
    {
        free(actor->name);
    }

    free(actor);
}

static bool process_single_message(void* data)
{
    edc_actor_t *actor = data;
    edc_actor_msg_t *msg = g_async_queue_try_pop(actor->mailbox);

    if (!msg)
        return G_SOURCE_REMOVE;

    actor->handler(actor, msg);
    return G_SOURCE_REMOVE;
}

void edc_actor_send(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    if (!actor || !msg || !actor->mailbox || !actor->handler || !actor->context)
        return;

    g_async_queue_push(actor->mailbox, msg);
    g_main_context_invoke(actor->context, process_single_message, actor);
}
