/* Every bubble that reaches a screen belongs to exactly one session.
 *
 * Historically `session_id` was only set on messages produced BY A TASK (so a
 * background session's output could be routed to its own transcript). Command
 * replies -- /help, /task stop, the "Job fired" notice -- had none, and the TUI
 * silently assigned them to whatever session happened to be displayed. That is
 * a UI-state guess, not an ownership fact; a durable chat_bubbles row keyed by
 * (session_id, seq) cannot be written without the real owner.
 *
 * This test pins the fix on the REAL path (channel actor on a real runner, the
 * same observer the TUI installs): edc_reply_to carries the inbound message's
 * session_id and edc_commands_reply puts it on the outgoing message.
 */
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "log.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_passed = 0, g_failed = 0;
#define CHECK(c, ...)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: ");                                                                                \
            fprintf(stderr, __VA_ARGS__);                                                                                 \
            fprintf(stderr, "\n");                                                                                         \
            g_failed++;                                                                                                    \
        }                                                                                                                  \
        else                                                                                                               \
        {                                                                                                                  \
            printf("    ok: ");                                                                                            \
            printf(__VA_ARGS__);                                                                                           \
            printf("\n");                                                                                                  \
            g_passed++;                                                                                                    \
        }                                                                                                                  \
    } while (0)

/* The observer runs on the runner thread, so capture is mutex-guarded and the
 * reader waits on a cond (no polling, no lost wakeup). */
static uv_mutex_t g_lock;
static uv_cond_t g_cond;
static sds g_last_sid = NULL; /* copy of the last reply's session_id, "" when absent */
static int g_last_feedback = 0; /* interface_feedback of the last reply */
static int g_replies = 0;

static void on_channel_send(void *instance, struct edc_channel_message *msg)
{
    (void)instance;
    uv_mutex_lock(&g_lock);
    if (g_last_sid)
        sdsfree(g_last_sid);
    g_last_sid = sdsnew(msg && msg->session_id ? msg->session_id : "");
    g_last_feedback = (msg && msg->interface_feedback) ? 1 : 0;
    g_replies++;
    uv_cond_signal(&g_cond);
    uv_mutex_unlock(&g_lock);
}

static bool wait_reply(int want, int timeout_ms)
{
    uv_mutex_lock(&g_lock);
    while (g_replies < want)
    {
        if (uv_cond_timedwait(&g_cond, &g_lock, (uint64_t)timeout_ms * 1000000ULL) != 0)
            break;
    }
    const bool ok = g_replies >= want;
    uv_mutex_unlock(&g_lock);
    return ok;
}

static sds last_sid(void)
{
    uv_mutex_lock(&g_lock);
    sds out = g_last_sid ? sdsdup(g_last_sid) : sdsempty();
    uv_mutex_unlock(&g_lock);
    return out;
}

static int last_feedback(void)
{
    uv_mutex_lock(&g_lock);
    int out = g_last_feedback;
    uv_mutex_unlock(&g_lock);
    return out;
}

int main(void)
{
    uv_mutex_init(&g_lock);
    uv_cond_init(&g_cond);

    edc_actor_t *channel = edc_channel_actor_new();
    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, channel);
    edc_actor_runner_start(runner);
    edc_channel_adapter_init(channel, NULL, on_channel_send);

    printf("-- command reply carries its session --\n");
    {
        struct edc_reply_to to = {.channel_actor = channel, .actor = NULL,
                                  .type = EDC_CHANNEL_MESSAGE_SYS_TYPE, .session_id = "s7"};
        edc_commands_reply(to, "reply text");
        CHECK(wait_reply(1, 2000), "the reply reached the observer");
        sds sid = last_sid();
        CHECK(strcmp(sid, "s7") == 0, "the reply carries session_id 's7' (got '%s')", sid);
        sdsfree(sid);
        /* Regression: with session_id now mandatory, a command reply can no
         * longer be identified by an empty one. It declares itself as
         * interface feedback so OutputRoutingPolicy still shows it -- otherwise
         * /task output, /help and every slash command go silent. */
        CHECK(last_feedback() == 1, "the command reply declares interface_feedback");
    }

    printf("-- no owner: honest absence, never a guessed display session --\n");
    {
        struct edc_reply_to to = {.channel_actor = channel, .actor = NULL,
                                  .type = EDC_CHANNEL_MESSAGE_SYS_TYPE, .session_id = NULL};
        edc_commands_reply(to, "no owner");
        CHECK(wait_reply(2, 2000), "the second reply reached the observer");
        sds sid = last_sid();
        CHECK(!*sid, "without an owner the field stays empty (got '%s')", sid);
        sdsfree(sid);
    }

    printf("-- structured history reply also carries the session --\n");
    {
        struct edc_reply_to to = {.channel_actor = channel, .actor = NULL,
                                  .type = EDC_CHANNEL_MESSAGE_TASK_HISTORY_TYPE, .session_id = "s9"};
        edc_commands_send_history(to, "{\"kind\":\"task\"}");
        CHECK(wait_reply(3, 2000), "the history reply reached the observer");
        sds sid = last_sid();
        CHECK(strcmp(sid, "s9") == 0, "history reply carries 's9' (got '%s')", sid);
        sdsfree(sid);
    }

    edc_actor_runner_stop(runner);
    edc_actor_runner_free(runner);
    edc_channel_actor_free(channel);
    uv_mutex_lock(&g_lock);
    if (g_last_sid)
        sdsfree(g_last_sid);
    uv_mutex_unlock(&g_lock);
    uv_cond_destroy(&g_cond);
    uv_mutex_destroy(&g_lock);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
