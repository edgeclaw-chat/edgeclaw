/* `/task fork <parent_id> <desc>`.
 *
 * Pins the child's landing, which now comes from the scheduler
 * (edc_scheduler_task_adopt) instead of this handler:
 *   ① the child leads a group of its own and takes the foreground + active pointer;
 *   ② it inherits the parent group's memory and its transcript snapshot;
 *   ③ desc is attached as the input turn (an empty desc attaches an empty turn);
 *   ④ it runs at the message end like any other task.
 *
 * harness copied from test_commands_task_new: drive the public edc_commands_handler
 * entry, then call edc_scheduler_schedule ourselves (the message end is the tick).
 * No channel actor ⇒ replies go nowhere, so assertions look at behaviour.
 */
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "utlist_fix.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stub for _run_loop pulled in by edc_commands (transitively linked). */
volatile int _run_loop = 1;

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}

static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_llm_calls++;
    g_llm_task = task;
}

static void drive_command(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    /* The command layer only attaches the input; running it is the message-end scan. */
    edc_scheduler_schedule(sched);
}

static struct edc_task *find_task_by_desc(struct edc_session *sess, const char *desc)
{
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (t->description && strcmp(t->description, desc) == 0)
                return t;
        }
    }
    return NULL;
}

/* The first user message the task would send. */
static const char *first_user_content(struct edc_turn *turn)
{
    if (!turn || turn->messages.tail == turn->messages.head)
        return NULL;
    cJSON *msg = turn->messages.data[turn->messages.head];
    return cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
}

int main(void)
{
    const char *ws = "/tmp/test_commands_task_fork_ws";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", ws, ws);
    if (system(cmd) != 0)
        return 1;

    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_session *sess = edc_session_manager_get_active(sm);
    CHECK(sess != NULL, "manager has an active session");
    if (!sess)
        return 1;

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);
    sched->send_llm = mock_send_llm;

    /* A parent to fork from (it stays in flight, so only the child is driven later). */
    drive_command(cfg, sm, sched, "/task new parent");
    struct edc_task *parent = g_llm_task;
    CHECK(parent != NULL && parent->group != NULL, "parent exists in a group");
    if (!parent || !parent->group)
        return 1;

    /* Give the parent group a memory and a transcript to snapshot: history[0] is the
     * system prompt, history[1] the first message. */
    parent->group->memory = sdsnew("PARENT-MEMORY");
    cJSON_AddItemToArray(parent->group->history, cJSON_CreateObject());
    cJSON *m1 = cJSON_CreateObject();
    cJSON_AddStringToObject(m1, "role", "user");
    cJSON_AddStringToObject(m1, "content", "PARENT-MESSAGE");
    cJSON_AddItemToArray(parent->group->history, m1);

    /* ── ① 落位: 新组 + 前台 + active ───────────────────────────── */

    TEST("/task fork makes the child lead its own group and take the foreground");
    g_llm_calls = 0;
    g_llm_task = NULL;
    char fork_cmd[256];
    snprintf(fork_cmd, sizeof(fork_cmd), "/task fork %s child work", parent->task_id);
    drive_command(cfg, sm, sched, fork_cmd);

    struct edc_task *child = find_task_by_desc(sess, "child work");
    CHECK(child != NULL, "the child exists");
    if (child)
    {
        CHECK(child->group != NULL && child->group != parent->group, "it leads a group of its own");
        CHECK(child->group && child->group->leader == child, "it is the leader of that group");
        CHECK(child->group && child->group_id && strcmp(child->group_id, child->task_id) == 0,
              "group id = child task id");
        CHECK(edc_scheduler_get_foreground(sess) == child->group, "the foreground moved onto it");
        CHECK(edc_session_get_active_task(sess) == child, "it is the active task");

        /* ── ② 继承 ─────────────────────────────────────────────── */

        CHECK(child->group && child->group->memory && strcmp(child->group->memory, "PARENT-MEMORY") == 0,
              "inherits the parent group's memory");
        CHECK(child->group && (size_t)cJSON_GetArraySize(child->group->history) == 2,
              "inherits the system prompt + the snapshot message");
        CHECK(child->turns.tail - child->turns.head == 1, "the snapshot turn is attached to the child");
        CHECK(child->forked_from == parent, "it remembers the parent");
        CHECK(child->fork_point_completion_seq == 0, "fork point = the parent group's watermark");

        /* ── ③ 输入 ─────────────────────────────────────────────── */

        CHECK(child->active_turn != NULL, "desc is attached as the input turn");
        const char *c = first_user_content(child->active_turn);
        CHECK(c && strcmp(c, "child work") == 0, "the input is desc");

        /* ── ④ 派发 ─────────────────────────────────────────────── */

        CHECK(child->current_request_id != NULL, "the child was dispatched at the message end");
    }
    CHECK(g_llm_calls == 1 && g_llm_task == child, "send_llm handed the request to the child");

    /* ── 只有 parent、没有 desc: 一条用法错误 ───────────────────────
     * (过去可以靠 `--prompt p1` 凑出第二个 token, 从而"无 desc 也建任务";
     *  --prompt 随 per-task prompt 一起删掉了, desc 是必需项。) */

    TEST("/task fork with no desc is a usage error");
    g_llm_calls = 0;
    g_llm_task = NULL;
    snprintf(fork_cmd, sizeof(fork_cmd), "/task fork %s", parent->task_id);
    drive_command(cfg, sm, sched, fork_cmd);

    CHECK(find_task_by_desc(sess, "") == NULL, "no child was created");
    CHECK(g_llm_calls == 0, "and nothing was dispatched");

    /* ── 未知 parent: 什么都不建 ──────────────────────────────────── */

    TEST("/task fork with an unknown parent builds nothing");
    struct edc_task_group *fg_before = edc_scheduler_get_foreground(sess);
    drive_command(cfg, sm, sched, "/task fork 999999 ghost");
    CHECK(find_task_by_desc(sess, "ghost") == NULL, "no task was created");
    CHECK(edc_scheduler_get_foreground(sess) == fg_before, "the foreground is untouched");

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
