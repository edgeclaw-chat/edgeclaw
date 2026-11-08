/* test_crontab_tool: the crontab TOOL's three actions, driven through edc_toolbox_exec.
 *
 * test_crontab_service.c covers the other half -- the shared service pump that fires a
 * due job. What it never touched is the tool itself: exec_add / exec_list / exec_remove
 * were entirely unexecuted, and with them every schedule kind (at / every / cron), the
 * timezone variants of each, the arg validation, and the three helpers that convert
 * between real unix time and "host-local wall clock encoded as unix time".
 *
 * Those helpers are static, so this file drives them the only honest way: through the
 * public entry the model would use. `crontab` with action=add and a cron expression runs
 * unix_to_local_wallclock -> cron_next -> local_wallclock_to_unix_ts, and the resulting
 * next_run is asserted against the wall clock the expression names. Stubbing would test
 * the stub.
 *
 * The toolbox is built with a NULL scheduler on purpose: that skips service registration
 * and makes emit_tool_message a no-op, so these cases test the tool, not the plumbing.
 */

/* strptime() is XSI, and this file computes the expected `at` instant the same way the
 * product does (strptime + mktime) rather than by hardcoding an epoch, so it has to be
 * declared. _GNU_SOURCE implies it.
 */
#define _GNU_SOURCE

#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_path.h"
#include "edc_scheduler.h"
#include "edc_service.h"
#include "edc_toolbox.h"
#include "log.h"
#include "sds.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "test_fs_util.h"

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

static sds g_result = NULL;
static EdcError *g_err = NULL;

static void capture_cb(struct edc_toolbox_call_ctx *ctx, sds result, EdcError *error)
{
    (void)ctx;
    if (g_result)
        sdsfree(g_result);
    if (g_err)
        edc_error_free(g_err);
    g_result = result; /* take ownership */
    g_err = error;
}

static void capture_reset(void)
{
    if (g_result)
    {
        sdsfree(g_result);
        g_result = NULL;
    }
    if (g_err)
    {
        edc_error_free(g_err);
        g_err = NULL;
    }
}

static char g_dir[512];

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);
    return cfg;
}

static void reset_dir(void)
{
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "    (warning: could not reset %s)\n", g_dir);
}

/* Run one crontab action and leave the outcome in g_result / g_err. */
static void run(struct edc_toolbox *tb, const char *args_json)
{
    capture_reset();
    cJSON *args = cJSON_Parse(args_json);
    if (!args)
    {
        fprintf(stderr, "    (bad test json: %s)\n", args_json);
        return;
    }
    struct edc_toolbox_call_ctx *ctx = edc_toolbox_call_ctx_new(tb, edc_channel_adapter, NULL, NULL, NULL);
    edc_toolbox_exec(ctx, "crontab", args, capture_cb);
    edc_toolbox_call_ctx_free(ctx);
    cJSON_Delete(args);
}

/* Writes jobs.json verbatim (used by the service-side cases further down). */
static void write_jobs_file(const char *json);

static cJSON *read_jobs_file(void)
{
    sds path = edc_path_join(g_dir, "jobs.json", NULL);
    cJSON *db = NULL;
    FILE *f = fopen(path, "r");
    if (f)
    {
        char buf[8192] = {0};
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        db = cJSON_Parse(buf);
    }
    sdsfree(path);
    return db;
}

/* The one job in the file (or NULL). */
static cJSON *only_job(cJSON *db)
{
    cJSON *jobs = db ? cJSON_GetObjectItem(db, "jobs") : NULL;
    return (jobs && cJSON_GetArraySize(jobs) > 0) ? cJSON_GetArrayItem(jobs, 0) : NULL;
}

static long long job_next_run(cJSON *job)
{
    cJSON *s = job ? cJSON_GetObjectItem(job, "schedule") : NULL;
    cJSON *n = s ? cJSON_GetObjectItem(s, "next_run_at_unix_ts") : NULL;
    return (n && cJSON_IsNumber(n)) ? (long long)n->valuedouble : -1;
}

static const char *job_str(cJSON *job, const char *a, const char *b)
{
    cJSON *x = job ? cJSON_GetObjectItem(job, a) : NULL;
    if (b)
        x = x ? cJSON_GetObjectItem(x, b) : NULL;
    return x ? cJSON_GetStringValue(x) : NULL;
}

/* ── add: every_seconds ─────────────────────────────────────────────────────── */
static void test_add_every(void)
{
    printf("\n── add: every_seconds ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    const long long before = (long long)time(NULL);
    run(tb, "{\"action\":\"add\",\"message\":\"drink water\",\"every_seconds\":3600}");
    const long long after = (long long)time(NULL);

    CHECK(g_err == NULL, "add with every_seconds succeeds");
    CHECK(g_result != NULL && strstr(g_result, "exit code: 0") != NULL, "the result reports success");

    cJSON *db = read_jobs_file();
    cJSON *job = only_job(db);
    CHECK(job != NULL, "the job was written to jobs.json");
    const long long next = job_next_run(job);
    CHECK(next >= before + 3600 && next <= after + 3600, "next_run is now + the interval");
    CHECK(job_str(job, "schedule", "kind") && strcmp(job_str(job, "schedule", "kind"), "every") == 0,
          "kind is every");
    CHECK(job_str(job, "job_id", NULL) != NULL && strlen(job_str(job, "job_id", NULL)) == 36,
          "a 36-character uuid was generated for the job id");

    /* No name was given, so it is derived from the message. */
    CHECK(job_str(job, "name", NULL) && strcmp(job_str(job, "name", NULL), "drink water") == 0,
          "the name defaults to the message");
    cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── add: an explicit name is kept, and a long message is truncated ─────────── */
static void test_add_name_and_truncation(void)
{
    printf("\n── add: name handling ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    run(tb, "{\"action\":\"add\",\"message\":\"m\",\"name\":\"my-job\",\"every_seconds\":60}");
    cJSON *db = read_jobs_file();
    CHECK(job_str(only_job(db), "name", NULL) && strcmp(job_str(only_job(db), "name", NULL), "my-job") == 0,
          "an explicit name is kept as given");
    cJSON_Delete(db);

    /* A message longer than 30 chars has no explicit name: the derived one is capped. */
    reset_dir();
    run(tb, "{\"action\":\"add\",\"message\":\"abcdefghijklmnopqrstuvwxyz0123456789\",\"every_seconds\":60}");
    db = read_jobs_file();
    const char *derived = job_str(only_job(db), "name", NULL);
    CHECK(derived != NULL && strlen(derived) <= 30, "a derived name is capped at 30 characters");
    cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── add: at (one-shot, host-local) ─────────────────────────────────────────── */
static void test_add_at(void)
{
    printf("\n── add: at ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    /* A fixed future wall clock. Without a tz it is HOST-local, so the expectation is
     * computed the same way the product does: strptime + mktime. */
    run(tb, "{\"action\":\"add\",\"message\":\"once\",\"at\":\"2031-05-01T12:00:00\"}");
    CHECK(g_err == NULL, "add with at succeeds");

    cJSON *db = read_jobs_file();
    cJSON *job = only_job(db);
    CHECK(job != NULL, "the one-shot job was written");

    struct tm want = {0};
#if defined(_WIN32)
    /* The product's own Windows branch parses the same format with sscanf (it has no
     * strptime in the CRT), so the expectation is built the same way here. */
    {
        int year, mon, day, hour, min, sec;
        if (sscanf("2031-05-01T12:00:00", "%d-%d-%dT%d:%d:%d",
                   &year, &mon, &day, &hour, &min, &sec) == 6)
        {
            want.tm_year = year - 1900;
            want.tm_mon = mon - 1;
            want.tm_mday = day;
            want.tm_hour = hour;
            want.tm_min = min;
            want.tm_sec = sec;
        }
    }
#else
    strptime("2031-05-01T12:00:00", "%Y-%m-%dT%H:%M:%S", &want);
#endif
    const long long expect = (long long)mktime(&want);
    CHECK(job_next_run(job) == expect, "next_run is that wall clock, interpreted as host-local time");
    CHECK(job_str(job, "schedule", "once") == NULL, "once is a bool, not a string");
    {
        cJSON *once = cJSON_GetObjectItem(cJSON_GetObjectItem(job, "schedule"), "once");
        CHECK(once && cJSON_IsTrue(once), "once is true for an at job");
    }
    cJSON_Delete(db);

    /* Malformed iso8601: refused, and nothing is written. */
    reset_dir();
    run(tb, "{\"action\":\"add\",\"message\":\"x\",\"at\":\"tomorrow at noon\"}");
    CHECK(g_err != NULL, "a malformed at is refused");
    CHECK(g_err && g_err->message && strstr(g_err->message, "not iso8601") != NULL,
          "and says the format is wrong");
    CHECK(read_jobs_file() == NULL, "no jobs.json is written for a refused add");

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── add: at with an explicit IANA timezone ────────────────────────────────── */
static void test_add_at_tz(void)
{
    printf("\n── add: at with a timezone ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    /* 12:00 in Shanghai is 04:00 UTC. Asserting the UTC hour is what proves the tz was
     * actually applied rather than the host zone. */
    run(tb, "{\"action\":\"add\",\"message\":\"x\",\"at\":\"2031-05-01T12:00:00\",\"tz\":\"Asia/Shanghai\"}");
    CHECK(g_err == NULL, "add with at + tz succeeds");

    cJSON *db = read_jobs_file();
    const long long next = job_next_run(only_job(db));
    struct tm utc = {0};
    const time_t t = (time_t)next;
    edc_gmtime_r(&t, &utc);
    CHECK(utc.tm_hour == 4 && utc.tm_min == 0, "the wall clock is mapped through the named zone (12:00 +08 -> 04:00 UTC)");
    CHECK(job_str(only_job(db), "schedule", "tz") && strcmp(job_str(only_job(db), "schedule", "tz"), "Asia/Shanghai") == 0,
          "the zone is recorded on the job");
    cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── add: cron expression (the three wall-clock conversions) ───────────────── */
static void test_add_cron(void)
{
    printf("\n── add: cron_expr ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    const time_t now = time(NULL);
    run(tb, "{\"action\":\"add\",\"message\":\"nightly\",\"cron_expr\":\"0 2 * * *\"}");
    CHECK(g_err == NULL, "add with a cron expression succeeds");

    cJSON *db = read_jobs_file();
    cJSON *job = only_job(db);
    CHECK(job != NULL, "the cron job was written");
    CHECK(job_str(job, "schedule", "kind") && strcmp(job_str(job, "schedule", "kind"), "cron") == 0, "kind is cron");

    const long long next = job_next_run(job);
    CHECK(next > (long long)now, "next_run is in the future");
    CHECK(next <= (long long)now + 24 * 3600 + 60, "and within the next day");
    /* The expression names 02:00 host-local, so the resolved instant must land exactly
     * there. This is the assertion that proves the local-wall-clock round trip works —
     * a naive implementation that treated the pseudo-timestamp as real unix time would
     * land at 02:00 UTC instead. */
    const time_t nt = (time_t)next;
    struct tm local = {0};
    edc_localtime_r(&nt, &local);
    CHECK(local.tm_hour == 2 && local.tm_min == 0 && local.tm_sec == 0,
          "next_run is 02:00:00 in host-local time, exactly as the expression says");
    cJSON_Delete(db);

    /* A bad expression is refused. */
    reset_dir();
    run(tb, "{\"action\":\"add\",\"message\":\"x\",\"cron_expr\":\"not a cron\"}");
    CHECK(g_err != NULL, "a malformed cron expression is refused");
    CHECK(read_jobs_file() == NULL, "and nothing is written");

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── add: cron with a timezone ─────────────────────────────────────────────── */
static void test_add_cron_tz(void)
{
    printf("\n── add: cron_expr with a timezone ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    run(tb, "{\"action\":\"add\",\"message\":\"x\",\"cron_expr\":\"0 2 * * *\",\"tz\":\"Asia/Shanghai\"}");
    CHECK(g_err == NULL, "add with cron + tz succeeds");

    cJSON *db = read_jobs_file();
    cJSON *job = only_job(db);
    CHECK(job != NULL && job_next_run(job) > 0, "the job has a next_run");

    /* 02:00 in Shanghai is 18:00 UTC the day before. */
    const time_t t = (time_t)job_next_run(job);
    struct tm utc = {0};
    edc_gmtime_r(&t, &utc);
    CHECK(utc.tm_hour == 18 && utc.tm_min == 0,
          "02:00 +08 resolved to 18:00 UTC, so the named zone was used");
    cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── add: argument validation ──────────────────────────────────────────────── */
static void test_add_validation(void)
{
    printf("\n── add: validation ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    run(tb, "{\"action\":\"add\",\"every_seconds\":60}");
    CHECK(g_err != NULL, "a missing message is refused");
    CHECK(g_err && strstr(g_err->message, "message is required") != NULL, "and names the missing message");

    run(tb, "{\"action\":\"add\",\"message\":\"m\"}");
    CHECK(g_err != NULL, "a missing schedule is refused");
    CHECK(g_err && strstr(g_err->message, "should be provided") != NULL, "and names the schedule requirement");

    run(tb, "{\"action\":\"add\",\"message\":\"m\",\"every_seconds\":60,\"tz\":\"Mars/Olympus\"}");
    CHECK(g_err != NULL, "an unknown timezone is refused");
    CHECK(g_err && strstr(g_err->message, "timezone is invalid") != NULL, "and says the zone is invalid");

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── list ──────────────────────────────────────────────────────────────────── */
static void test_list(void)
{
    printf("\n── list ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    /* Nothing scheduled yet: an empty list, not an error. */
    run(tb, "{\"action\":\"list\"}");
    CHECK(g_err == NULL, "list with no jobs.json is not an error");
    CHECK(g_result != NULL && sdslen(g_result) == 0, "and returns an empty result");

    /* A jobs.json that has no "jobs" key at all. */
    {
        sds p = edc_path_join(g_dir, "jobs.json", NULL);
        FILE *f = fopen(p, "w");
        if (f)
        {
            fputs("{\"something_else\":1}", f);
            fclose(f);
        }
        sdsfree(p);
    }
    run(tb, "{\"action\":\"list\"}");
    CHECK(g_err == NULL, "list on a jobs.json without a jobs key is not an error");
    CHECK(g_result != NULL && sdslen(g_result) == 0, "and returns an empty result");

    /* With a job in it, both the name and the id are reported. */
    reset_dir();
    run(tb, "{\"action\":\"add\",\"message\":\"m\",\"name\":\"reporter\",\"every_seconds\":60}");
    run(tb, "{\"action\":\"list\"}");
    CHECK(g_err == NULL, "list succeeds");
    CHECK(g_result && strstr(g_result, "job name: reporter") != NULL, "the list shows the job name");
    CHECK(g_result && strstr(g_result, "job id: ") != NULL, "the list shows the job id");

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── remove ────────────────────────────────────────────────────────────────── */
static void test_remove(void)
{
    printf("\n── remove ──\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    run(tb, "{\"action\":\"remove\"}");
    CHECK(g_err != NULL, "remove without a job_id is refused");
    CHECK(g_err && strstr(g_err->message, "job_id is not provided") != NULL, "and names the missing id");

    run(tb, "{\"action\":\"remove\",\"job_id\":\"whatever\"}");
    CHECK(g_err != NULL, "remove with no jobs.json is refused");
    CHECK(g_err && strstr(g_err->message, "no jobs database found") != NULL, "and says there is no database");

    /* Add one, capture its id, remove it. */
    reset_dir();
    run(tb, "{\"action\":\"add\",\"message\":\"m\",\"every_seconds\":60}");
    cJSON *db = read_jobs_file();
    char id[64] = {0};
    {
        const char *jid = job_str(only_job(db), "job_id", NULL);
        if (jid)
            snprintf(id, sizeof(id), "%s", jid);
    }
    cJSON_Delete(db);
    CHECK(id[0] != '\0', "the added job has an id to remove");

    char args[256];
    snprintf(args, sizeof(args), "{\"action\":\"remove\",\"job_id\":\"%s\"}", id);
    run(tb, args);
    CHECK(g_err == NULL, "removing an existing job succeeds");
    CHECK(g_result && strstr(g_result, "Remove job") != NULL && strstr(g_result, "exit code: 0") != NULL,
          "and reports success");

    db = read_jobs_file();
    cJSON *jobs = db ? cJSON_GetObjectItem(db, "jobs") : NULL;
    CHECK(jobs && cJSON_GetArraySize(jobs) == 0, "the job is gone from jobs.json");
    cJSON_Delete(db);

    /* Removing an id that is not there: still "succeeds" (idempotent), nothing breaks. */
    run(tb, args);
    CHECK(g_err == NULL, "removing an absent id reports success (idempotent)");
    db = read_jobs_file();
    jobs = db ? cJSON_GetObjectItem(db, "jobs") : NULL;
    CHECK(jobs && cJSON_GetArraySize(jobs) == 0, "and leaves the list empty");
    cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

/* ── the service half: a due CRON job advances (not just "every") ────────────
 * test_crontab_service.c covers a due "every" job. The cron branch of advance_job (and
 * its tz variant) needs its own case, and so do the scan guards: a jobs file with no
 * jobs key, and a schedule with no timestamp.
 */
static void write_jobs_file(const char *json)
{
    sds p = edc_path_join(g_dir, "jobs.json", NULL);
    FILE *f = fopen(p, "w");
    if (f)
    {
        fputs(json, f);
        fclose(f);
    }
    sdsfree(p);
}

static void test_service_advances_due_cron_job(void)
{
    printf("\n── service: a due cron job advances ──\n");
    reset_dir();
    /* Already due (a minute in the past), recurring, no tz -> advance_job's local branch. */
    const long long past = (long long)time(NULL) - 60;
    char json[512];
    snprintf(json, sizeof(json),
             "{\"jobs\":[{\"job_id\":\"c1\",\"name\":\"n\",\"message\":\"m\",\"schedule\":"
             "{\"kind\":\"cron\",\"tz\":\"\",\"cron_expr\":\"0 2 * * *\",\"once\":false,"
             "\"next_run_at_unix_ts\":%lld}}]}",
             past);
    write_jobs_file(json);

    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_toolbox *tb = edc_toolbox_new(cfg, edc_scheduler_get_actor(sched), NULL);

    edc_services_pump(sched, 1000);

    cJSON *db = read_jobs_file();
    cJSON *job = only_job(db);
    CHECK(job != NULL, "the recurring cron job is kept");
    const long long next = job_next_run(job);
    CHECK(next > (long long)time(NULL), "next_run advanced into the future");
    const time_t nt = (time_t)next;
    struct tm local = {0};
    edc_localtime_r(&nt, &local);
    CHECK(local.tm_hour == 2 && local.tm_min == 0, "and it advanced to the next 02:00 host-local");
    cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_service_advances_due_cron_job_tz(void)
{
    printf("\n── service: a due cron job with a tz advances ──\n");
    reset_dir();
    const long long past = (long long)time(NULL) - 60;
    char json[512];
    snprintf(json, sizeof(json),
             "{\"jobs\":[{\"job_id\":\"c2\",\"name\":\"n\",\"message\":\"m\",\"schedule\":"
             "{\"kind\":\"cron\",\"tz\":\"Asia/Shanghai\",\"cron_expr\":\"0 2 * * *\",\"once\":false,"
             "\"next_run_at_unix_ts\":%lld}}]}",
             past);
    write_jobs_file(json);

    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_toolbox *tb = edc_toolbox_new(cfg, edc_scheduler_get_actor(sched), NULL);

    edc_services_pump(sched, 1000);

    cJSON *db = read_jobs_file();
    cJSON *job = only_job(db);
    CHECK(job != NULL, "the job is kept");
    const time_t t = (time_t)job_next_run(job);
    struct tm utc = {0};
    edc_gmtime_r(&t, &utc);
    CHECK(utc.tm_hour == 18 && utc.tm_min == 0,
          "next_run advanced to 18:00 UTC, i.e. 02:00 Shanghai -- the named zone was used");
    cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

static void test_service_scan_guards(void)
{
    printf("\n── service: scan guards ──\n");

    /* The service is registered with a 3600s interval, so a pump of 1s does NOT reach
     * the scan. Every case here pumps past the interval: without that the scan never
     * runs and a "does not crash" assertion would pass while testing nothing. */
    const long long past_interval = 3600LL * 1000 + 1000;

    /* No jobs.json at all: the scan returns quietly. */
    reset_dir();
    struct edc_config *cfg = make_config();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_toolbox *tb = edc_toolbox_new(cfg, edc_scheduler_get_actor(sched), NULL);
    edc_services_pump(sched, past_interval);
    CHECK(read_jobs_file() == NULL, "a scan with no jobs.json writes nothing");
    edc_toolbox_free(tb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);

    /* A jobs.json without a jobs key. */
    reset_dir();
    write_jobs_file("{\"note\":\"no jobs array here\"}");
    cfg = make_config();
    sched = edc_scheduler_new(cfg);
    tb = edc_toolbox_new(cfg, edc_scheduler_get_actor(sched), NULL);
    edc_services_pump(sched, past_interval);
    {
        cJSON *db = read_jobs_file();
        CHECK(db != NULL && cJSON_GetObjectItem(db, "note") != NULL,
              "a scan on a jobs.json without a jobs key leaves the file alone");
        cJSON_Delete(db);
    }
    edc_toolbox_free(tb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);

    /* One job with no timestamp (skipped) and one due job (advanced). The second job
     * advancing is what proves the scan actually ran; the first staying exactly as it
     * was proves the no-timestamp guard held instead of crashing or inventing a value. */
    reset_dir();
    const long long due = (long long)time(NULL) - 60;
    char json[900];
    snprintf(json, sizeof(json),
             "{\"jobs\":[{\"job_id\":\"j-no-ts\",\"name\":\"n1\",\"message\":\"m\","
             "\"schedule\":{\"kind\":\"every\",\"every_seconds\":60,\"once\":false}},"
             "{\"job_id\":\"j-due\",\"name\":\"n2\",\"message\":\"m\","
             "\"schedule\":{\"kind\":\"every\",\"tz\":\"\",\"once\":false,\"every_seconds\":3600,"
             "\"next_run_at_unix_ts\":%lld}}]}",
             due);
    write_jobs_file(json);

    cfg = make_config();
    sched = edc_scheduler_new(cfg);
    tb = edc_toolbox_new(cfg, edc_scheduler_get_actor(sched), NULL);
    edc_services_pump(sched, 1000); /* the due job re-armed the service */

    {
        cJSON *db = read_jobs_file();
        cJSON *jobs = db ? cJSON_GetObjectItem(db, "jobs") : NULL;
        cJSON *no_ts = jobs ? cJSON_GetArrayItem(jobs, 0) : NULL;
        cJSON *advanced = jobs ? cJSON_GetArrayItem(jobs, 1) : NULL;
        CHECK(jobs && cJSON_GetArraySize(jobs) == 2, "both jobs survive the scan");
        CHECK(no_ts && job_next_run(no_ts) == -1, "the job with no timestamp is untouched");
        CHECK(advanced && job_next_run(advanced) > (long long)time(NULL),
              "the due job was advanced -- so the scan really ran");
        cJSON_Delete(db);
    }
    edc_toolbox_free(tb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ── add: a jobs.json that exists but has no "jobs" key ────────────────────── */
static void test_add_to_file_without_jobs_key(void)
{
    printf("\n── add: jobs.json without a jobs key ──\n");
    reset_dir();
    /* A jobs.json can legitimately be missing the key (truncated write, an older shape,
     * or something the model wrote). Adding must still land the job: the alternative is
     * a silent loss, where the tool reports "done" and the job is nowhere. */
    write_jobs_file("{\"note\":\"no jobs array here\"}");

    struct edc_config *cfg = make_config();
    struct edc_toolbox *tb = edc_toolbox_new(cfg, NULL, NULL);

    run(tb, "{\"action\":\"add\",\"message\":\"must survive\",\"every_seconds\":60}");
    CHECK(g_err == NULL, "add into a jobs.json without a jobs key does not error");

    cJSON *db = read_jobs_file();
    cJSON *jobs = db ? cJSON_GetObjectItem(db, "jobs") : NULL;
    CHECK(jobs != NULL && cJSON_IsArray(jobs), "a jobs array is created");
    CHECK(jobs && cJSON_GetArraySize(jobs) == 1, "the new job is actually in it");
    CHECK(jobs && job_str(cJSON_GetArrayItem(jobs, 0), "message", NULL) &&
              strcmp(job_str(cJSON_GetArrayItem(jobs, 0), "message", NULL), "must survive") == 0,
          "and it is the job that was added");
    cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace(); /* disposable store for anything that writes state */
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_crontab_tool_%ld",
             (tmp && *tmp) ? tmp : "/tmp", (long)getpid());

    printf("workspace: %s\n", g_dir);

    test_add_every();
    test_add_name_and_truncation();
    test_add_at();
    test_add_at_tz();
    test_add_cron();
    test_add_cron_tz();
    test_add_validation();
    test_add_to_file_without_jobs_key();
    test_list();
    test_remove();
    test_service_advances_due_cron_job();
    test_service_advances_due_cron_job_tz();
    test_service_scan_guards();

    capture_reset();
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", g_dir);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
