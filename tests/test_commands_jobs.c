/* tests/test_commands_jobs.c -- `/jobs`: the scheduled jobs, visible to a person.
 *
 * The schedules are data (<workspace>/jobs.json) that the crontab tool writes and a
 * background service ticks. These tests drive the command entry and check what a user can
 * observe: a list that does not touch anything, `run` that puts the job's instruction
 * through the input pipeline (and leaves the schedule alone), `rm` that removes THAT job,
 * and the two ways an id can be wrong (no match / ambiguous) changing nothing.
 *
 * Command replies go nowhere here (no channel actor), so the assertions are on the tasks
 * and on the file -- the two things that actually change.
 *
 * Harness copied from test_commands_task_new. */
#include "edc_actor.h"
#include "test_ws.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
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

volatile int _run_loop = 1; /* stub for the symbol edc_commands pulls in */

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
static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)task;
    (void)request_id;
    g_llm_calls++;
}

/* The command layer injects the job's text with edc_actor_send, which queues it and
 * wakes the actor -- in production the loop drains the mailbox. Give the scheduler actor
 * a loop here (edc_actor_runner_add) and turn it once, so the test exercises the real
 * delivery path instead of calling the handler behind the API's back. */
static edc_actor_runner_t *g_runner = NULL;

static void pump_mailbox(struct edc_scheduler *sched)
{
    if (g_runner)
        uv_run(&g_runner->loop, UV_RUN_NOWAIT);
    edc_scheduler_schedule(sched);
}

static void drive_command(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                          const char *content)
{
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_commands_handler(cfg, sm, sched, (struct edc_reply_to){.type = cm->type}, cm);
    edc_scheduler_schedule(sched);
    pump_mailbox(sched); /* a `/jobs run` or a task spawn may have queued work */
}

static int count_tasks(struct edc_session *sess)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        n++;
    }
    return n;
}

/* The input of the task that was created last (what the model would see). */
static const char *last_task_input(struct edc_session *sess)
{
    struct edc_task *last = NULL;
    struct edc_task_group *g;
    DL_FOREACH(sess->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        last = t;
    }
    if (!last || !last->active_turn || last->active_turn->msg_count == 0)
        return NULL;
    cJSON *msg = edc_turn_msg_at(last->active_turn, 0);
    return cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
}

static void write_jobs(const char *workspace, const char *json)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/jobs.json", workspace);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fputs(json, f);
    fclose(f);
}

/* How many jobs the file holds now, and whether |name| is one of them. */
static int jobs_in_file(const char *workspace, const char *name, bool *found)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/jobs.json", workspace);
    *found = false;
    if (!edc_file_is_regular(path))
        return -1;

    EdcError *error = NULL;
    sds content = edc_file_read(path, &error);
    if (error)
        edc_error_free(error);
    if (!content)
        return -1;

    cJSON *db = cJSON_Parse(content);
    sdsfree(content);
    if (!db)
        return -1;

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");
    const int n = cJSON_GetArraySize(jobs);
    for (int i = 0; i < n; i++)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        const char *job_name = cJSON_GetStringValue(cJSON_GetObjectItem(job, "name"));
        if (name && job_name && strcmp(job_name, name) == 0)
            *found = true;
    }
    cJSON_Delete(db);
    return n;
}

int main(void)
{
    const char *ws = edc_test_workspace();
    char cmd[512];
    /* workspace created once; the store stays open on it */


    static const char *kJobs =
        "{\"jobs\":["
        "{\"name\":\"nightly\",\"job_id\":\"aaaa1111-0000-0000-0000-000000000000\","
        "\"message\":\"run the nightly build\","
        "\"schedule\":{\"kind\":\"cron\",\"cron_expr\":\"0 3 * * *\",\"tz\":\"Asia/Shanghai\","
        "\"next_run_at_unix_ts\":1893456000}},"
        "{\"name\":\"probe\",\"job_id\":\"bbbb2222-0000-0000-0000-000000000000\","
        "\"message\":\"check the logs\","
        "\"schedule\":{\"kind\":\"every\",\"every_seconds\":600,\"tz\":\"\","
        "\"next_run_at_unix_ts\":1893456000}}"
        "]}";
    write_jobs(ws, kJobs);

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
    /* A loop, so edc_actor_send is not the no-op it is for an unattached actor. */
    g_runner = edc_actor_runner_new();
    edc_actor_runner_add(g_runner, edc_scheduler_get_actor(sched));

    /* ── 列表：什么都不改 ─────────────────────────────────────────── */

    TEST("/jobs lists without creating a task or touching the file");
    {
        const int before = count_tasks(sess);
        g_llm_calls = 0;
        drive_command(cfg, sm, sched, "/jobs");
        CHECK(count_tasks(sess) == before, "no task was created");
        CHECK(g_llm_calls == 0, "and nothing ran");
        bool found = false;
        CHECK(jobs_in_file(ws, "nightly", &found) == 2 && found, "the file still holds both jobs");
    }

    /* ── run：把指令送进输入流水线，不改日程 ──────────────────────── */

    TEST("/jobs run <id-prefix> fires that job's instruction as a message");
    {
        const int before = count_tasks(sess);
        g_llm_calls = 0;
        drive_command(cfg, sm, sched, "/jobs run aaaa1111");

        CHECK(count_tasks(sess) == before + 1, "a task was created from the job");
        const char *input = last_task_input(sess);
        CHECK(input && strstr(input, "run the nightly build") != NULL, "carrying the job's instruction");
        CHECK(input && strstr(input, "Timer finished") != NULL, "in the same shape the timer produces");
        CHECK(g_llm_calls == 1, "and it was dispatched to the model");

        bool found = false;
        CHECK(jobs_in_file(ws, "nightly", &found) == 2 && found, "the schedule itself is unchanged");
    }

    /* ── rm：删的是那一条 ────────────────────────────────────────── */

    TEST("/jobs rm <id-prefix> removes exactly that job");
    {
        drive_command(cfg, sm, sched, "/jobs rm aaaa1111");
        bool nightly = true, probe = false;
        const int left = jobs_in_file(ws, "nightly", &nightly);
        CHECK(left == 1, "one job is left");
        CHECK(!nightly, "the one named was removed");
        jobs_in_file(ws, "probe", &probe);
        CHECK(probe, "and the other one is untouched");
    }

    /* ── 错的 id：什么都不改 ─────────────────────────────────────── */

    TEST("an unknown id changes nothing");
    {
        const int before = count_tasks(sess);
        drive_command(cfg, sm, sched, "/jobs rm zzzz");
        bool probe = false;
        CHECK(jobs_in_file(ws, "probe", &probe) == 1 && probe, "the file is untouched");
        drive_command(cfg, sm, sched, "/jobs run zzzz");
        CHECK(count_tasks(sess) == before, "and no task was created");
    }

    TEST("an ambiguous prefix changes nothing");
    {
        write_jobs(ws,
                   "{\"jobs\":["
                   "{\"name\":\"one\",\"job_id\":\"cccc1111-0000\",\"message\":\"m1\","
                   "\"schedule\":{\"kind\":\"every\",\"every_seconds\":60,\"next_run_at_unix_ts\":1893456000}},"
                   "{\"name\":\"two\",\"job_id\":\"cccc2222-0000\",\"message\":\"m2\","
                   "\"schedule\":{\"kind\":\"every\",\"every_seconds\":60,\"next_run_at_unix_ts\":1893456000}}"
                   "]}");
        const int before = count_tasks(sess);
        drive_command(cfg, sm, sched, "/jobs rm cccc");
        bool one = false;
        CHECK(jobs_in_file(ws, "one", &one) == 2 && one, "both jobs are still there");
        drive_command(cfg, sm, sched, "/jobs run cccc");
        CHECK(count_tasks(sess) == before, "and nothing ran");
    }

    /* ── 没有 jobs.json：不是错误，也不炸 ────────────────────────── */

    TEST("no jobs.json: list is empty and run/rm do nothing");
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/jobs.json", ws);
        remove(path);

        const int before = count_tasks(sess);
        drive_command(cfg, sm, sched, "/jobs");
        CHECK(count_tasks(sess) == before, "no task");

        drive_command(cfg, sm, sched, "/jobs run whatever");
        CHECK(count_tasks(sess) == before, "still no task");
        CHECK(!edc_file_is_regular(path), "and no file was invented by a read");
    }

    /* store still open: nothing to remove */

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
