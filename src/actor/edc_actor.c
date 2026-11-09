#include "edc_actor.h"
#include <stdlib.h>
#include <unistd.h>

/* Atomic 64-bit increment, spelled once for every compiler.
 *
 * `__sync_add_and_fetch` is a GCC/Clang builtin: MSVC does not have it, so on Windows the
 * reference stayed unresolved until link time (`LNK2019: unresolved external symbol
 * __sync_add_and_fetch`) rather than failing where it is written. _InterlockedIncrement64 is
 * the MSVC equivalent and returns the incremented value, like the builtin. */
#if defined(_MSC_VER)
#include <intrin.h>
static __inline uint64_t edc_atomic_inc_u64(volatile uint64_t *p)
{
    return (uint64_t)_InterlockedIncrement64((volatile __int64 *)p);
}
#else
#define edc_atomic_inc_u64(p) __sync_add_and_fetch((p), 1)
#endif

static void *generate_request_id(void)
{
    static volatile uint64_t counter = 0;
    uint64_t id = edc_atomic_inc_u64(&counter);
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
    if (!runner)
        return NULL;
    uv_loop_init(&runner->loop);
    runner->running = 0;
    runner->thread_started = 0;
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

static void actor_thread_func(void *data)
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
    /* Before anything can try to stop us: the join must not depend on the thread having
     * got as far as setting `running`. */
    runner->thread_started = 1;

    for (int i = 0; i < runner->actor_count; ++i)
    {
        edc_actor_t *actor = runner->actors[i];
        edc_actor_send(actor, edc_actor_msg_new(BC_ACTOR_MSG_START, NULL, NULL, NULL, NULL));
    }
}

void edc_actor_runner_stop(edc_actor_runner_t *runner)
{
    /* `thread_started`, not `running`: see the field's comment. Joining a thread that has
     * already exited is fine and still required (it reaps it). */
    if (!runner || !runner->thread_started)
        return;

    uv_stop(&runner->loop);
    if (runner->actor_count > 0)
        uv_async_send(&runner->actors[0]->wakeup);
    uv_thread_join(&runner->thread);
    runner->thread_started = 0;
}

void edc_actor_runner_free(edc_actor_runner_t *runner)
{
    if (!runner)
        return;

    if (runner->thread_started)
    {
        edc_actor_runner_stop(runner);
    }

    /* Closing the handles is not enough on its own. uv_close() is asynchronous: the handle
     * is only unlinked when the loop runs again, and until then uv_loop_close() returns
     * UV_EBUSY. That matters beyond a leaked handle, because uv_spawn() registers a child
     * watcher in libuv's PROCESS-GLOBAL signal tree -- free the loop while that watcher is
     * still linked and the tree keeps a pointer into freed memory, so the next uv_spawn()
     * anywhere in this process reads it. ASan reports that as a heap-use-after-free in
     * uv__signal_compare, several frames away from anything this file owns.
     *
     * The thread path had this drain inside actor_thread_func(); a runner whose loop is
     * pumped by the caller (no edc_actor_runner_start(), which is how the shell tool's
     * tests drive it) never went through it.
     *
     * Bounded rather than UV_RUN_DEFAULT: a child that is still running would keep a
     * default run going indefinitely, and shutdown must not hang. */
    uv_walk(&runner->loop, close_all_handles_cb, NULL);
    for (int i = 0; i < 50 && uv_run(&runner->loop, UV_RUN_NOWAIT) != 0; i++)
    {
    }

    if (uv_loop_close(&runner->loop) != 0)
    {
        /* Something is still registered -- an active request, or a handle whose close the
         * drain above could not complete. Leaving the loop allocated leaks it, which is
         * the lesser failure: freeing it is what corrupts libuv's global state, and the
         * crash would surface in an unrelated caller. */
        return;
    }

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
        if (!new_actors)
            return;
        runner->actors = new_actors;
        runner->actor_capacity = new_cap;
    }
    runner->actors[runner->actor_count++] = actor;
}

edc_actor_msg_t *edc_actor_msg_new_sub_type(edc_actor_msg_type_t type, int sub_type, edc_actor_t *from,
                                            void *request_id, void *data, edc_actor_data_free_fn fn)
{
    edc_actor_msg_t *msg = calloc(1, sizeof(edc_actor_msg_t));
    if (!msg)
        return NULL;
    msg->type = type;
    msg->sub_type = sub_type;
    msg->data = data;
    msg->from = from;
    msg->request_id = request_id ? request_id : generate_request_id();
    msg->data_free_fn = fn;

    return msg;
}

edc_actor_msg_t *edc_actor_msg_new(edc_actor_msg_type_t type, edc_actor_t *from, void *request_id, void *data,
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
    if (!actor)
        return NULL;
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
