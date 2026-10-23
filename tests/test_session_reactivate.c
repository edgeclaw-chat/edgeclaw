/* Regression test for the crash on re-activating a session and running a task.
 *
 * Scenario (reported): run a task in session A, switch to session B, run a task
 * there, switch back to A and run a task -> SIGSEGV in edc_llm_actor_handler.
 *
 * Mechanism (fixed):
 *   - On STOP the scheduler records every committed turn message into the
 *     group history *by reference* (edc_task_group_history_add_ref), then moves
 *     those same cJSON objects into session->messages (edc_session_add_turn).
 *   - /session switch re-activated the target session and called
 *     edc_session_reload() on it. Reload deleted and rebuilt session->messages,
 *     freeing the very objects the live group->history still pointed at.
 *   - The next LLM request walked group->history and dereferenced the freed
 *     messages -> heap-use-after-free / SIGSEGV.
 *
 * The fix removes the reload from the session lifecycle entirely: an activated
 * session keeps its in-memory messages (the LLM context comes from
 * group->history anyway, never from session->messages).
 *
 * This test drives the real /session switch command handler between two
 * sessions that both have committed task history, then dereferences the group
 * history the same way edc_app_cb_send_llm does (a use-after-free would abort
 * under ASan).
 */

#include "edc_channel_actor.h"
#include "test_ws.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t _run_loop = 1;

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        log_info("  TEST: %s", name);                                                                                  \
    } while (0)
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: %s", msg);                                                                            \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            test_passed++;                                                                                             \
        }                                                                                                              \
    } while (0)

#define TEST_WS edc_test_workspace()

/* Simulate the STOP path of sched_handle_llm_resp for a finished turn:
 * record the turn's messages into group history *by reference*, then commit
 * the turn to the task (P4: the turn owns its messages; session->messages no
 * longer receives them). */
static void commit_turn_into_group(struct edc_session *sess, struct edc_task *task, const char *user,
                                   const char *assistant)
{
    struct edc_turn *turn = edc_turn_new();
    edc_turn_add_user_message(turn, user);
    cJSON *am = cJSON_CreateObject();
    cJSON_AddStringToObject(am, "role", "assistant");
    cJSON_AddStringToObject(am, "content", assistant);
    edc_turn_add_assistant_message(turn, am);
    task->active_turn = turn;

    if (task->group && task->active_turn)
    {
        for (int qi = 0; qi < task->active_turn->msg_count; qi++)
        {
            cJSON *item = edc_turn_msg_at(task->active_turn, qi);
            edc_task_group_history_add_ref(task->group, item);
        }
    }
    edc_task_commit_active_turn(task);
}

/* Run a full (stub) task in |sess| on |sched|: create task + group, make it the
 * foreground, and commit one turn. Returns the group. */
static struct edc_task_group *run_task(struct edc_scheduler *sched, struct edc_session *sess, const char *desc)
{
    struct edc_task *t = edc_scheduler_task_new(sched, sess, desc, edc_channel_adapter);
    edc_task_await_exec(t);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, t);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, t);
    commit_turn_into_group(sess, t, desc, "done");
    edc_task_mark_completed(t);
    return g;
}

/* Walk a group's history exactly like edc_app_cb_send_llm, dereferencing every
 * message. Returns how many entries were readable; on a dangling entry this is
 * a use-after-free. */
static int touch_group_history(struct edc_task_group *g)
{
    int seen = 0;
    size_t total = g->history ? cJSON_GetArraySize(g->history) : 0;
    for (size_t i = 0; i < total; i++)
    {
        cJSON *m = cJSON_GetArrayItem(g->history, i);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *content = cJSON_GetObjectItem(m, "content");
        if (role && cJSON_IsString(role) && content && cJSON_IsString(content))
            seen++;
    }
    return seen;
}

/* Drive the real "/session switch <target>" command handler. */
static void switch_to(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                      edc_actor_t *channel_actor, const char *target)
{
    sds cmd = sdscatprintf(sdsempty(), "/session switch %s", target);
    struct edc_channel_message *msg =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsdup(cmd));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.channel_actor = channel_actor, .type = msg->type}, msg);
    edc_channel_message_free(msg);
    sdsfree(cmd);
}

static int count_messages_containing(struct edc_session *s, const char *needle)
{
    /* P4: 消息归属 turn; 遍历所有 turn 的队列统计。 */
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(s->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            for (size_t i = t->turns.head; i < t->turns.tail; i++)
            {
                struct edc_turn *turn = (struct edc_turn *)t->turns.data[i];
                if (!turn)
                    continue;
                for (int j = 0; j < turn->msg_count; j++)
                {
                    cJSON *content = cJSON_GetObjectItem((cJSON *)edc_turn_msg_at(turn, j), "content");
                    if (content && cJSON_IsString(content) && strstr(content->valuestring, needle))
                        n++;
                }
            }
            if (t->active_turn)
            {
                for (int j = 0; j < t->active_turn->msg_count; j++)
                {
                    cJSON *content = cJSON_GetObjectItem((cJSON *)edc_turn_msg_at(t->active_turn, j), "content");
                    if (content && cJSON_IsString(content) && strstr(content->valuestring, needle))
                        n++;
                }
            }
        }
    }
    return n;
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    char cmd[512];
    /* no rm: the store is open on this workspace */
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", TEST_WS);
    system(cmd);

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(TEST_WS);

    TEST("switch away and back keeps group history valid");

    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sa = edc_session_manager_get_active(sm);
    CHECK(sa != NULL, "manager created a default session");
    sa->state_loaded = true;

    /* Second session; create_ex makes it active, so put A back in front. */
    struct edc_session *sb = edc_session_manager_create_ex(sm, "B", NULL);
    CHECK(sb != NULL, "second session created");
    sm->active_index = edc_session_manager_index_of(sm, sa->session_id);

    edc_actor_t *channel_actor = edc_channel_actor_new();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);

    /* Task in A (committed turn: group history references A->messages). */
    struct edc_task_group *ga = run_task(sched, sa, "task-a-1");
    CHECK(count_messages_containing(sa, "task-a-1") == 1, "A's committed turn is in A->messages");
    CHECK(touch_group_history(ga) == 2, "A's group history readable before switching");

    /* Switch A -> B, run a task, then switch back B -> A. */
    switch_to(cfg, sm, sched, channel_actor, sb->session_id);
    CHECK(edc_session_manager_get_active(sm) == sb, "B is the active session after switch");
    struct edc_task_group *gb = run_task(sched, sb, "task-b-1");
    CHECK(touch_group_history(gb) == 2, "B's group history readable");

    switch_to(cfg, sm, sched, channel_actor, sa->session_id);
    CHECK(edc_session_manager_get_active(sm) == sa, "A is the active session again");
    CHECK(sa->state_loaded, "A still flagged loaded (in-memory authoritative)");
    CHECK(count_messages_containing(sa, "task-a-1") == 1, "A->messages survived re-activation");

    /* Running a task in A builds its LLM request from ga->history. */
    struct edc_task_group *ga2 = run_task(sched, sa, "task-a-2");
    CHECK(touch_group_history(ga) == 2, "A's original group history still valid after re-activation");
    CHECK(touch_group_history(ga2) == 2, "A's new group history valid");

    edc_scheduler_free(sched);
    edc_channel_actor_free(channel_actor);
    edc_session_manager_free(sm);
    edc_config_free(cfg);
    /* no rm: the store is open on this workspace */

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
