/* The write funnel: every chat bubble is persisted exactly once, on the actor
 * thread, with the same bytes the TUI will render. Control messages (snapshot,
 * tree-changed, history payload, page reply) must NOT become bubbles.
 *
 * This drives the REAL channel actor on a REAL runner (the same path the
 * product uses) with a capture observer, then reads chat_bubbles back through
 * the store API. A UI thread writing the same line again, or a control reply
 * slipping into the whitelist, shows up here as a duplicate / extra row.
 */
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_chat_bubble_line.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_state_store.h"
#include "log.h"
#include "sds.h"
#include "test_ws.h"

#include <inttypes.h>
#include <stdbool.h>
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

static uv_mutex_t g_lock;
static uv_cond_t g_cond;
static int g_seen = 0;

static void on_send(void *instance, struct edc_channel_message *msg)
{
    (void)instance;
    (void)msg;
    uv_mutex_lock(&g_lock);
    g_seen++;
    uv_cond_signal(&g_cond);
    uv_mutex_unlock(&g_lock);
}

static bool wait_seen(int want, int timeout_ms)
{
    uv_mutex_lock(&g_lock);
    while (g_seen < want)
    {
        if (uv_cond_timedwait(&g_cond, &g_lock, (uint64_t)timeout_ms * 1000000ULL) != 0)
            break;
    }
    const bool ok = g_seen >= want;
    uv_mutex_unlock(&g_lock);
    return ok;
}

static void send_bubble(edc_actor_t *channel, edc_channel_message_type type, const char *sid,
                        const char *content)
{
    struct edc_channel_message *msg =
        edc_channel_message_new(edc_channel_adapter, type, sdsnew(content));
    msg->session_id = sdsnew(sid);
    msg->render_pref = 0;
    edc_actor_send(channel,
                   edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_SEND, channel, NULL, msg,
                                     (edc_actor_data_free_fn)edc_channel_message_free));
}

/* A task's output that is NOT its session's active task (a curate/extract system
 * task, or a superseded task). The frontend's Silent gate will not show it, so
 * it must not be persisted either -- otherwise the restart replay shows ~50
 * lines of system chatter the user never saw, pushing their real reply out of
 * the loaded page. It must still reach the adapter: display filtering is the
 * dispatcher's job, and this funnel only decides what is durable. */
static void send_task_message_ex(edc_actor_t *channel, edc_channel_message_type type, const char *sid,
                                 const char *task_id, bool from_active, bool feedback)
{
    struct edc_channel_message *msg = edc_channel_message_new(edc_channel_adapter, type, sdsnew("system chatter"));
    msg->session_id = sdsnew(sid);
    msg->task_id = sdsnew(task_id);
    msg->from_active_task = from_active;
    msg->interface_feedback = feedback;
    edc_actor_send(channel,
                   edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_SEND, channel, NULL, msg,
                                     (edc_actor_data_free_fn)edc_channel_message_free));
}

static void send_task_message(edc_actor_t *channel, edc_channel_message_type type, const char *sid,
                              const char *task_id, bool from_active)
{
    send_task_message_ex(channel, type, sid, task_id, from_active, false);
}

int main(void)
{
    edc_test_workspace(); /* opens the store on a real temp workspace */
    uv_mutex_init(&g_lock);
    uv_cond_init(&g_cond);

    edc_actor_t *channel = edc_channel_actor_new();
    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, channel);
    edc_actor_runner_start(runner);
    edc_channel_adapter_init(channel, NULL, on_send);

    const char *sid = "s42";

    printf("-- five bubble types, one row each --\n");
    send_bubble(channel, EDC_CHANNEL_MESSAGE_USER_TYPE, sid, "hello");
    send_bubble(channel, EDC_CHANNEL_MESSAGE_TOOL_TYPE, sid, "tool output");
    send_bubble(channel, EDC_CHANNEL_MESSAGE_THOUGHT_TYPE, sid, "thinking");
    send_bubble(channel, EDC_CHANNEL_MESSAGE_RESP_TYPE, sid, "reply");
    send_bubble(channel, EDC_CHANNEL_MESSAGE_SYS_TYPE, sid, "system note");
    CHECK(wait_seen(5, 3000), "all five bubbles reached the observer");
    CHECK(edc_chat_bubble_count(sid) == 5, "exactly 5 rows written, got %" PRId64,
          edc_chat_bubble_count(sid));

    printf("-- control messages are not bubbles --\n");
    {
        struct edc_reply_to to = {.channel_actor = channel, .actor = NULL,
                                  .type = EDC_CHANNEL_MESSAGE_TASK_HISTORY_TYPE, .session_id = sid};
        edc_commands_send_history(to, "{\"kind\":\"task\"}");
        edc_channel_publish((struct edc_reply_to){.channel_actor = channel, .actor = NULL,
                                                  .type = EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE, .session_id = sid},
                            sid, sdsnew("{\"groups\":[]}"));
        CHECK(wait_seen(7, 3000), "both control replies reached the observer");
        CHECK(edc_chat_bubble_count(sid) == 5, "control messages added no rows, got %" PRId64,
              edc_chat_bubble_count(sid));
    }

    printf("-- non-active task output: not persisted, still delivered --\n");
    {
        g_seen = 0;
        const int64_t before = edc_chat_bubble_count(sid);
        /* A curate task (not the session's active task) streams thoughts/tools. */
        send_task_message(channel, EDC_CHANNEL_MESSAGE_THOUGHT_TYPE, sid, "1073", false);
        send_task_message(channel, EDC_CHANNEL_MESSAGE_TOOL_TYPE, sid, "1073", false);
        CHECK(wait_seen(2, 3000), "both non-active task messages reached the observer");
        CHECK(edc_chat_bubble_count(sid) == before, "no rows written for a non-active task, got %" PRId64,
              edc_chat_bubble_count(sid) - before);

        /* A command reply carries a task id but is interface feedback: even from a
         * non-active task it must be written (the frontend shows interface feedback
         * regardless of the active-task gate). */
        send_task_message_ex(channel, EDC_CHANNEL_MESSAGE_SYS_TYPE, sid, "1073", false, true);
        CHECK(wait_seen(3, 3000), "the feedback message reached the observer");
        CHECK(edc_chat_bubble_count(sid) == before + 1, "interface feedback was written despite non-active, got %" PRId64,
              edc_chat_bubble_count(sid) - before);

        /* The active task's output is still persisted. */
        send_task_message(channel, EDC_CHANNEL_MESSAGE_RESP_TYPE, sid, "1071", true);
        CHECK(wait_seen(4, 3000), "the active task message reached the observer");
        CHECK(edc_chat_bubble_count(sid) == before + 2, "the active task wrote one row, got %" PRId64,
              edc_chat_bubble_count(sid) - before);
    }

    printf("-- the stored line is byte-identical to what the UI renders --\n");
    {
        char *expected = edc_chat_bubble_encode("sys", 0, "", "", "a\\n escaped newline", "");
        CHECK(expected != NULL, "shared encoder produced a line");
        struct edc_chat_bubble_row *rows = NULL;
        int n = 0;
        CHECK(edc_chat_bubble_page(sid, INT64_MAX, 10, &rows, &n) && n == 7, "page reads 7 rows");
        if (n == 7 && expected)
        {
            CHECK(edc_test_bubble_row_line(&rows[4]) && strcmp(edc_test_bubble_row_line(&rows[4]), "sys\t0\t\t\tsystem note\t") == 0,
                  "the sys row's encoded shape is exactly kind/pref/task/header/content");
            CHECK(edc_test_bubble_row_line(&rows[4]) && strstr(edc_test_bubble_row_line(&rows[4]), "system note") != NULL, "content survives verbatim");
            /* Feedback (task 1073) and the active-task reply (1071) were written; the
             * non-active chatter from task 1073 was not. Verify by task id in the
             * encoded kind/pref/task/header/content line. */
            CHECK(edc_test_bubble_row_line(&rows[5]) && strstr(edc_test_bubble_row_line(&rows[5]), "\t1073\t") != NULL,
                  "the non-active task's interface feedback row is present");
            CHECK(edc_test_bubble_row_line(&rows[6]) && strstr(edc_test_bubble_row_line(&rows[6]), "\t1071\t") != NULL,
                  "the active task's row is present and newest");
        }
        edc_chat_bubble_rows_free(rows, n);
        free(expected);
    }

    printf("-- a bubble with no session_id is refused, never guessed --\n");
    {
        struct edc_channel_message *msg =
            edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_SYS_TYPE, sdsnew("orphan"));
        edc_actor_send(channel,
                       edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_SEND, channel, NULL, msg,
                                         (edc_actor_data_free_fn)edc_channel_message_free));
        CHECK(wait_seen(5, 3000), "the orphan reached the observer");
        CHECK(edc_chat_bubble_count(sid) == 7, "the orphan was not written anywhere");
    }

    edc_actor_runner_stop(runner);
    edc_actor_runner_free(runner);
    edc_channel_actor_free(channel);
    uv_cond_destroy(&g_cond);
    uv_mutex_destroy(&g_lock);
    edc_test_store_down(NULL);

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
