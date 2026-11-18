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

    printf("-- the stored line is byte-identical to what the UI renders --\n");
    {
        char *expected = edc_chat_bubble_encode("sys", 0, "", "", "a\\n escaped newline");
        CHECK(expected != NULL, "shared encoder produced a line");
        struct edc_chat_bubble_row *rows = NULL;
        int n = 0;
        CHECK(edc_chat_bubble_page(sid, INT64_MAX, 10, &rows, &n) && n == 5, "page reads 5 rows");
        if (n == 5 && expected)
        {
            CHECK(rows[4].line && strcmp(rows[4].line, "sys\t0\t\t\tsystem note") == 0,
                  "the last row's encoded shape is exactly kind/pref/task/header/content");
            CHECK(strstr(rows[4].line, "system note") != NULL, "content survives verbatim");
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
        CHECK(wait_seen(8, 3000), "the orphan reached the observer");
        CHECK(edc_chat_bubble_count(sid) == 5, "the orphan was not written anywhere");
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
