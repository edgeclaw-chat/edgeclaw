#include "edc_actor.h"
#include <stdlib.h>
#include <unistd.h>

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

static void* generate_request_id(void)
{
    static volatile uint64_t counter = 0;
    uint64_t id = __sync_add_and_fetch(&counter, 1);
    return (void *)(uintptr_t)id;
}

static void on_actor_wakeup(uv_async_t *handle)
{
    edc_actor_t *actor = container_of(handle, edc_actor_t, wakeup);
    edc_actor_msg_t *msg;

    uv_mutex_lock(&actor->mailbox_lock);
    while ((msg = edc_queue_pop(&actor->mailbox)) != NULL)
    {
        uv_mutex_unlock(&actor->mailbox_lock);
        actor->handler(actor, msg);
        uv_mutex_lock(&actor->mailbox_lock);
    }
    uv_mutex_unlock(&actor->mailbox_lock);
}

edc_actor_runner_t *edc_actor_runner_new()
{
    edc_actor_runner_t *runner = calloc(1, sizeof(edc_actor_runner_t));
    if (!runner) return NULL;
    uv_loop_init(&runner->loop);
    runner->running = 0;
    runner->actors = NULL;
    runner->actor_count = 0;
    runner->actor_capacity = 0;
    return runner;
}

static void close_all_handles_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
    {
        uv_close(handle, NULL);
    }
}

static void actor_thread_func(void* data)
{
    edc_actor_runner_t *runner = data;

    runner->running = 1;
    uv_run(&runner->loop, UV_RUN_DEFAULT);

    uv_walk(&runner->loop, close_all_handles_cb, NULL);
    uv_run(&runner->loop, UV_RUN_DEFAULT);

    runner->running = 0;
    return;
}

void edc_actor_runner_start(edc_actor_runner_t *runner)
{
    if (!runner)
        return;
    uv_thread_create(&runner->thread, actor_thread_func, runner);

    for (int i = 0; i < runner->actor_count; ++i)
    {
        edc_actor_t *actor = runner->actors[i];
        edc_actor_send(actor, edc_actor_msg_new(BC_ACTOR_MSG_START, NULL, NULL, NULL, NULL));
    }
}

void edc_actor_runner_stop(edc_actor_runner_t *runner)
{
    if (!runner || !runner->running)
        return;

    uv_stop(&runner->loop);
    if (runner->actor_count > 0)
        uv_async_send(&runner->actors[0]->wakeup);
    uv_thread_join(&runner->thread);
}

void edc_actor_runner_free(edc_actor_runner_t *runner)
{
    if (!runner)
        return;

    if (runner->running)
    {
        edc_actor_runner_stop(runner);
    }

    uv_loop_close(&runner->loop);
    free(runner->actors);
    free(runner);
}

void edc_actor_runner_add(edc_actor_runner_t *runner, edc_actor_t *actor)
{
    actor->loop = &runner->loop;
    uv_async_init(&runner->loop, &actor->wakeup, on_actor_wakeup);

    if (runner->actor_count >= runner->actor_capacity)
    {
        int new_cap = runner->actor_capacity ? runner->actor_capacity * 2 : 8;
        edc_actor_t **new_actors = realloc(runner->actors, (size_t)new_cap * sizeof(edc_actor_t *));
        if (!new_actors) return;
        runner->actors = new_actors;
        runner->actor_capacity = new_cap;
    }
    runner->actors[runner->actor_count++] = actor;
}

edc_actor_msg_t *edc_actor_msg_new_sub_type(edc_actor_msg_type_t type, int sub_type, edc_actor_t *from, void* request_id,
                                          void *data, edc_actor_data_free_fn fn)
{
    edc_actor_msg_t *msg = calloc(1, sizeof(edc_actor_msg_t));
    if (!msg) return NULL;
    msg->type = type;
    msg->sub_type = sub_type;
    msg->data = data;
    msg->from = from;
    msg->request_id = request_id ? request_id : generate_request_id();
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
    edc_actor_t *actor = calloc(1, sizeof(edc_actor_t));
    if (!actor) return NULL;
    actor->name = sdsnew(name);
    actor->handler = handler;
    actor->state = state;
    edc_queue_init(&actor->mailbox);
    uv_mutex_init(&actor->mailbox_lock);
    actor->loop = NULL;
    return actor;
}

void edc_actor_free(edc_actor_t *actor)
{
    if (!actor)
        return;

    uv_mutex_lock(&actor->mailbox_lock);
    edc_actor_msg_t *msg;
    while ((msg = edc_queue_pop(&actor->mailbox)) != NULL)
    {
        uv_mutex_unlock(&actor->mailbox_lock);
        edc_actor_msg_free(msg);
        uv_mutex_lock(&actor->mailbox_lock);
    }
    uv_mutex_unlock(&actor->mailbox_lock);

    edc_queue_destroy(&actor->mailbox);
    uv_mutex_destroy(&actor->mailbox_lock);
    sdsfree(actor->name);
    free(actor);
}

void edc_actor_send(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    if (!actor || !msg || !actor->handler || !actor->loop)
        return;

    uv_mutex_lock(&actor->mailbox_lock);
    edc_queue_push(&actor->mailbox, msg);
    uv_mutex_unlock(&actor->mailbox_lock);

    uv_async_send(&actor->wakeup);
}
