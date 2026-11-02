/* test_commands_report: the commands that only REPORT state.
 *
 * /session list|info|save|stop, /group list, /task list|status|output|history and
 * /skill list were the largest cluster of never-executed functions in edc_commands.c.
 * They share a shape -- read the session/group/task tree, format it, send one reply --
 * and, crucially, their whole value IS that reply text. A harness that only checked
 * "the command returned true" would pass with every one of them printing nothing, so
 * this file captures what actually goes to the channel.
 *
 * The capture is the real path, not a stub: a real channel actor on a real actor runner,
 * with the adapter's observer installed via edc_channel_adapter_init -- the same hook the
 * TUI uses to receive BC_ACTOR_MSG_CHANNEL_SEND. edc_commands_reply() addresses
 * to.channel_actor, so passing that actor is what makes the reply observable.
 *
 * Tasks are created through `/task new` (the public path) with a spy send_llm, the way
 * test_commands_task_new does it, so the tree these commands walk is the tree the
 * product would have.
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
#include "log.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

volatile int _run_loop = 1;

static int g_passed = 0, g_failed = 0;

#define CHECK(c, m) do { \
    if (!(c)) { printf("    FAIL: %s\n", m); g_failed++; } \
    else { printf("    ok: %s\n", m); g_passed++; } \
} while (0)

/* ── capturing the reply ──────────────────────────────────────────────────────
 * The observer runs on the actor runner thread, so the capture is guarded and the
 * reader waits on the same mutex+cond (no lost wakeup, no polling). */
static uv_mutex_t g_lock;
static uv_cond_t g_cond;
static sds g_last = NULL;   /* content of the most recent reply */
static int g_replies = 0;   /* how many arrived in total */

static void on_channel_send(void *instance, struct edc_channel_message *msg)
{
    (void)instance;
    if (!msg || !msg->content)
        return;
    uv_mutex_lock(&g_lock);
    if (g_last)
        sdsfree(g_last);
    g_last = sdsdup(msg->content);
    g_replies++;
    uv_cond_signal(&g_cond);
    uv_mutex_unlock(&g_lock);
}

/* Wait until at least |want| replies have arrived since the last reset. Returns false on
 * timeout, which is what turns "the command said nothing" into a test failure instead of
 * a silently passing assertion. */
static bool wait_replies(int want, int timeout_ms)
{
    bool ok = false;
    uv_mutex_lock(&g_lock);
    while (g_replies < want)
    {
        if (uv_cond_timedwait(&g_cond, &g_lock, (uint64_t)timeout_ms * 1000000ULL) != 0)
            break; /* timed out */
    }
    ok = g_replies >= want;
    uv_mutex_unlock(&g_lock);
    return ok;
}

/* Snapshot of the last reply text (caller frees). NULL when nothing arrived. */
static sds last_reply(void)
{
    sds out = NULL;
    uv_mutex_lock(&g_lock);
    if (g_last)
        out = sdsdup(g_last);
    uv_mutex_unlock(&g_lock);
    return out;
}

static void reset_capture(void)
{
    uv_mutex_lock(&g_lock);
    if (g_last)
    {
        sdsfree(g_last);
        g_last = NULL;
    }
    g_replies = 0;
    uv_mutex_unlock(&g_lock);
}

/* ── the harness ──────────────────────────────────────────────────────────── */

static edc_actor_t *g_chan = NULL;
static edc_actor_runner_t *g_runner = NULL;

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)task;
    (void)request_id;
}

/* Run one line through the public command entry and wait for its reply. The handler takes
 * ownership of the message, so it is not freed here. */
static bool drive(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                  const char *content)
{
    reset_capture();
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched,
                         (struct edc_reply_to){.channel_actor = g_chan, .type = cm->type}, cm);
    /* The actor handler schedules at the end of every message; do the same here. */
    edc_scheduler_schedule(sched);
    return wait_replies(1, 5000);
}

/* First task in the session whose description is |desc| (the newest one wins on a clash,
 * which is what a just-created task is). */
static struct edc_task *find_task(struct edc_session *sess, const char *desc)
{
    struct edc_task *found = NULL;
    struct edc_task_group *g;
    for (g = sess ? sess->groups : NULL; g; g = g->next)
    {
        struct edc_task *t;
        for (t = g->members; t; t = t->group_next)
        {
            if (t->description && strcmp(t->description, desc) == 0)
                found = t;
        }
    }
    return found;
}

/* Drive and return the reply text (caller frees); NULL when no reply arrived. */
static sds say(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
               const char *content)
{
    if (!drive(cfg, sm, sched, content))
        return NULL;
    return last_reply();
}

#define HAS(hay, needle) ((hay) != NULL && strstr((hay), (needle)) != NULL)

int main(void)
{
    edc_test_workspace(); /* the store, for anything that persists state */

    uv_mutex_init(&g_lock);
    uv_cond_init(&g_cond);

    /* A real channel actor so replies are observable. */
    g_chan = edc_channel_actor_new();
    g_runner = edc_actor_runner_new();
    edc_actor_runner_add(g_runner, g_chan);
    edc_actor_runner_start(g_runner);
    edc_channel_adapter_init(g_chan, NULL, on_channel_send);

    char ws[] = "/tmp/edc_cmd_report_XXXXXX";
    if (!mkdtemp(ws))
    {
        perror("mkdtemp");
        return 2;
    }

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    if (!sm)
    {
        printf("FAIL: cannot build the session manager\n");
        return 2;
    }
    sched->sessions = sm;
    sched->ctx = edc_session_manager_get_active(sm);
    sched->send_llm = mock_send_llm;

    struct edc_session *sess = edc_session_manager_get_active(sm);

    printf("\n── /session list ──\n");
    {
        sds r = say(cfg, sm, sched, "/session list");
        printf("    reply: %.150s\n", r ? r : "(none)");
        CHECK(r != NULL, "the reply arrives on the channel actor (not dropped)");
        CHECK(r != NULL && sdslen(r) > 0, "and is not empty");
        sdsfree(r);
    }

    printf("\n── /session info ──\n");
    {
        sds r = say(cfg, sm, sched, "/session info");
        printf("    reply: %.150s\n", r ? r : "(none)");
        CHECK(r != NULL && sdslen(r) > 0, "/session info reports something");
        /* The info text names the session; the id must appear or the report is useless. */
        CHECK(r != NULL && sess && HAS(r, edc_id_str(sess->session_id)), "it names the active session's id");
        sdsfree(r);
    }

    printf("\n── /session save ──\n");
    {
        sds r = say(cfg, sm, sched, "/session save");
        CHECK(r != NULL && HAS(r, "saved"), "/session save confirms it persisted");
        sdsfree(r);
    }

    printf("\n── /task new, then the reporting commands ──\n");
    {
        /* Create a task through the public path so the tree is a real one. */
        reset_capture();
        struct edc_channel_message *cm =
            edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew("/task new report-me"));
        edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.channel_actor = g_chan, .type = cm->type}, cm);
        edc_scheduler_schedule(sched);
        (void)wait_replies(1, 5000);

        struct edc_task_group *g = NULL;
        struct edc_task *t = NULL;
        for (g = sess->groups; g && !t; g = g->next)
        {
            for (t = g->members; t; t = t->group_next)
                break;
        }
        CHECK(t != NULL, "a task exists after /task new");
        if (!t)
            goto out;

        char task_id[32];
        snprintf(task_id, sizeof(task_id), "%lld", (long long)t->task_id);

        printf("\n── /task list ──\n");
        {
            sds r = say(cfg, sm, sched, "/task list");
            printf("    reply: %.200s\n", r ? r : "(none)");
            CHECK(r != NULL && HAS(r, "## Tasks"), "/task list renders a task list");
            CHECK(r != NULL && HAS(r, "report-me"), "and it names the task's description");
            sdsfree(r);
        }

        printf("\n── /group list ──\n");
        {
            sds r = say(cfg, sm, sched, "/group list");
            printf("    reply: %.200s\n", r ? r : "(none)");
            CHECK(r != NULL && HAS(r, "## Groups"), "/group list renders a group list");
            /* Formatted from the TASK's own group_id rather than from a re-walk of the
             * session's group list: it is the id that matters, and it does not depend on
             * edc_id_str's rotating buffer or on which group the walk landed in. */
            char gid[32];
            snprintf(gid, sizeof(gid), "%lld", (long long)t->group_id);
            CHECK(r != NULL && HAS(r, gid), "and it names the group id");
            sdsfree(r);
        }

        printf("\n── /task status ──\n");
        {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "/task status %s", task_id);
            sds r = say(cfg, sm, sched, cmd);
            printf("    reply: %.200s\n", r ? r : "(none)");
            CHECK(r != NULL && sdslen(r) > 0, "/task status reports something");
            /* Nothing in flight -> no block at all (no empty section). */
            CHECK(r != NULL && !HAS(r, "running"), "a task with nothing in flight prints no running block");
            sdsfree(r);
        }

        /* A task mid-turn: "state" says it is busy, and the running block says with WHAT.
         * The name alone (`exec_shell`) is not the answer -- the arguments are.
         *
         * Two calls go into the turn, but only the one that is STILL in flight is printed: the
         * ledger is the definition, so a call whose result came back (removed from it) is
         * history, not "what is it doing now". */
        printf("\n── /task status: only the call in flight ──\n");
        {
            CHECK(t->active_turn != NULL, "the task has an in-flight turn to record a call in");
            if (t->active_turn)
            {
                cJSON *done_args = cJSON_CreateObject();
                cJSON_AddStringToObject(done_args, "command", "echo settled-call");
                edc_turn_add_assistant_tool_call(t->active_turn, "call_report_done", "exec_shell", done_args);
                edc_task_tool_call_add(t, "call_report_done", "exec_shell", done_args);
                edc_task_tool_call_mark_executing(t, "call_report_done");
                /* The result arrived: the call leaves the ledger, so it must stop being shown. */
                CHECK(edc_task_tool_call_remove(t, "call_report_done"), "a settled call leaves the ledger");

                cJSON *args = cJSON_CreateObject();
                cJSON_AddStringToObject(args, "command", "ls -la /tmp/report");
                edc_turn_add_assistant_tool_call(t->active_turn, "call_report_1", "exec_shell", args);
                edc_task_tool_call_add(t, "call_report_1", "exec_shell", args);
                edc_task_tool_call_mark_executing(t, "call_report_1");

                char cmd[64];
                snprintf(cmd, sizeof(cmd), "/task status %s", task_id);
                sds r = say(cfg, sm, sched, cmd);
                printf("    reply:\n%s\n", r ? r : "(none)\n");
                CHECK(r != NULL && HAS(r, "running"), "the running block appears");
                CHECK(r != NULL && HAS(r, "exec_shell"), "it names the tool");
                CHECK(r != NULL && HAS(r, "ls -la /tmp/report"), "and the detailed command, not just the name");
                CHECK(r != NULL && HAS(r, "1 tool call in flight"), "it counts what is in flight");
                CHECK(r != NULL && !HAS(r, "settled-call"), "and does not repeat the call that already returned");
                sdsfree(r);

                /* The call returns -> the block goes away by itself. */
                edc_task_tool_call_remove(t, "call_report_1");
                r = say(cfg, sm, sched, cmd);
                CHECK(r != NULL && !HAS(r, "running"), "once the last call returns, the block is gone");
                sdsfree(r);

                /* Put it back so the later commands see the same tree (does not matter, but keeps
                 * the fixture honest about what exists). */
                edc_task_tool_call_add(t, "call_report_1", "exec_shell", args);
                cJSON_Delete(args);
                cJSON_Delete(done_args);
            }
        }

        /* A task id that does not exist: refused, and said so. */
        {
            sds r = say(cfg, sm, sched, "/task status 999999");
            CHECK(r != NULL && HAS(r, "not found"), "/task status on an unknown id says not found");
            sdsfree(r);
        }

        printf("\n── /task output ──\n");
        {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "/task output %s", task_id);
            sds r = say(cfg, sm, sched, cmd);
            printf("    reply: %.150s\n", r ? r : "(none)");
            /* A freshly created task has produced nothing yet. */
            CHECK(r != NULL, "/task output replies");
            sdsfree(r);
        }
        {
            sds r = say(cfg, sm, sched, "/task output 999999");
            CHECK(r != NULL && HAS(r, "not found"), "/task output on an unknown id says not found");
            sdsfree(r);
        }
        {
            sds r = say(cfg, sm, sched, "/task output");
            CHECK(r != NULL && HAS(r, "Usage"), "/task output without an id prints usage");
            sdsfree(r);
        }

        printf("\n── /task history ──\n");
        {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "/task history %s", task_id);
            sds r = say(cfg, sm, sched, cmd);
            printf("    reply: %.150s\n", r ? r : "(none)");
            CHECK(r != NULL && sdslen(r) > 0, "/task history replies");
            sdsfree(r);
        }
        {
            /* --json is an operand, and may stand anywhere among the words. */
            char cmd[80];
            snprintf(cmd, sizeof(cmd), "/task history --json %s", task_id);
            sds r = say(cfg, sm, sched, cmd);
            printf("    reply: %.150s\n", r ? r : "(none)");
            CHECK(r != NULL && sdslen(r) > 0, "/task history --json replies");
            sdsfree(r);
        }
        {
            sds r = say(cfg, sm, sched, "/task history");
            CHECK(r != NULL && HAS(r, "Usage"), "/task history without an id prints usage");
            sdsfree(r);
        }
        {
            /* Two ids: the operand splitter refuses rather than guessing. */
            sds r = say(cfg, sm, sched, "/task history 1 2");
            CHECK(r != NULL && HAS(r, "Usage"), "/task history with two ids prints usage");
            sdsfree(r);
        }

        printf("\n── /task bg, /group history, /task dep ──\n");
        {
            /* The group is the foreground one, so this really moves it to the background
             * (the other branch only says "already in the background"). */
            sds r = say(cfg, sm, sched, "/task bg");
            printf("    reply: %.150s\n", r ? r : "(none)");
            CHECK(r != NULL && HAS(r, "No foreground group"), "/task bg clears the foreground");
            sdsfree(r);
        }
        {
            /* Now that group is in the background: the same command on its task says so. */
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "/task bg %s", task_id);
            sds r = say(cfg, sm, sched, cmd);
            printf("    reply: %.150s\n", r ? r : "(none)");
            CHECK(r != NULL && HAS(r, "already in the background"), "/task bg on a background task says so");
            sdsfree(r);
        }
        {
            char gid[32];
            snprintf(gid, sizeof(gid), "%lld", (long long)t->group_id);
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "/group history %s", gid);
            sds r = say(cfg, sm, sched, cmd);
            printf("    reply: %.150s\n", r ? r : "(none)");
            CHECK(r != NULL && sdslen(r) > 0, "/group history replies");
            sdsfree(r);
        }
        {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "/task dep %s", task_id);
            sds r = say(cfg, sm, sched, cmd);
            printf("    reply: %.150s\n", r ? r : "(none)");
            CHECK(r != NULL && sdslen(r) > 0, "/task dep replies");
            sdsfree(r);
            sds r2 = say(cfg, sm, sched, "/task dep 999999");
            CHECK(r2 != NULL && HAS(r2, "not found"), "/task dep on an unknown id says not found");
            sdsfree(r2);
        }
        {
            /* outstanding_deps() only runs when a task still WAITS on something, and its
             * loop body only runs when at least one edge survives the drop. So: a task
             * that depends on two others, then drop one -- the reply must name the one
             * that is left. */
            char c1[64], cmd[256];
            snprintf(c1, sizeof(c1), "/task new third-task");
            (void)drive(cfg, sm, sched, c1);

            struct edc_task *third = find_task(sess, "third-task");
            CHECK(third != NULL, "a third task exists to depend on");
            if (third)
            {
                snprintf(cmd, sizeof(cmd), "/task new second-task -d %lld -d %lld",
                         (long long)t->task_id, (long long)third->task_id);
                (void)drive(cfg, sm, sched, cmd);

                struct edc_task *second = find_task(sess, "second-task");
                CHECK(second != NULL, "the dependent task exists");
                CHECK(second && edc_task_awaits_deps(second), "and it waits for its dependencies");

                if (second)
                {
                    snprintf(cmd, sizeof(cmd), "/task dep %lld --drop %lld",
                             (long long)second->task_id, (long long)t->task_id);
                    sds r = say(cfg, sm, sched, cmd);
                    printf("    reply: %.200s\n", r ? r : "(none)");
                    CHECK(r != NULL && HAS(r, "dropped 1 dependency edge"), "dropping one edge is reported");
                    /* The remaining edge is the third task: the reply has to name it. */
                    char left[32];
                    snprintf(left, sizeof(left), "%lld", (long long)third->task_id);
                    CHECK(r != NULL && HAS(r, "still waits for") && HAS(r, left),
                          "and the outstanding dependency is named");
                    sdsfree(r);
                }
            }
        }
    }

    printf("\n── /skill (list) ──\n");
    {
        /* The list is the BARE `/skill`: `/skill list` means "show the skill named
         * list", which is a different (and also worth pinning) path. */
        sds r = say(cfg, sm, sched, "/skill");
        printf("    reply: %.200s\n", r ? r : "(none)");
        CHECK(r != NULL && sdslen(r) > 0, "/skill replies");
        CHECK(r != NULL && (HAS(r, "## Skills") || HAS(r, "No skill")),
              "and reports either the skills or their absence");
        sdsfree(r);
    }
    {
        /* A name that is not a skill: refused by name, not silently swallowed. */
        sds r = say(cfg, sm, sched, "/skill list");
        CHECK(r != NULL && HAS(r, "No skill"), "/skill <unknown> says no such skill");
        sdsfree(r);
    }

out:
    printf("\n── /session stop ──\n");
    {
        sds r = say(cfg, sm, sched, "/session stop");
        printf("    reply: %.150s\n", r ? r : "(none)");
        CHECK(r != NULL && sdslen(r) > 0, "/session stop replies");
        sdsfree(r);
    }
    {
        /* It takes no argument: an argument is refused with the pointer to /session remove. */
        sds r = say(cfg, sm, sched, "/session stop 1");
        CHECK(r != NULL && HAS(r, "remove"), "/session stop with an argument points at /session remove");
        sdsfree(r);
    }

    reset_capture();
    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
