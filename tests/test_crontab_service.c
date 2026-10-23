/* crontab migrated onto the shared background service: a due job fires and the
 * schedule advances, all driven by the single service pump (jobs.json format kept). */

#include "edc_compat.h"
#include "test_ws.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_service.h"
#include "edc_toolbox.h"
#include "log.h"
#include "sds.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

static void write_jobs(const char *dir, const char *json)
{
    sds path = edc_path_join(dir, "jobs.json", NULL);
    FILE *f = fopen(path, "w");
    if (f)
    {
        fputs(json, f);
        fclose(f);
    }
    sdsfree(path);
}

static cJSON *read_jobs(const char *dir)
{
    sds path = edc_path_join(dir, "jobs.json", NULL);
    cJSON *db = NULL;
    FILE *f = fopen(path, "r");
    if (f)
    {
        char buf[4096] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        db = cJSON_Parse(buf);
    }
    sdsfree(path);
    return db;
}

static struct edc_config *make_config(const char *dir)
{
    mkdir(dir, 0755);
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(dir);
    return cfg;
}

/* A one-shot ("at") job that is already due: firing it must REMOVE it. */
static void test_due_once_job_fires_and_is_removed(void)
{
    TEST("crontab service: a due one-shot job fires on the pump and is dropped");
    const char *dir = "/tmp/test_crontab_service_a";
    struct edc_config *cfg = make_config(dir);
    long long past = (long long)time(NULL) - 10;
    char json[512];
    snprintf(json, sizeof(json),
             "{\"jobs\":[{\"job_id\":\"j1\",\"name\":\"n\",\"message\":\"m\","
             "\"schedule\":{\"kind\":\"at\",\"tz\":\"\",\"once\":true,\"next_run_at_unix_ts\":%lld}}]}",
             past);
    write_jobs(dir, json);

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_toolbox *tb = edc_toolbox_new(cfg, edc_scheduler_get_actor(sched), NULL);

    CHECK(edc_service_find(sched, "crontab") != NULL, "the crontab service is registered");

    edc_services_pump(sched, 1000); /* the service was armed for the due job */

    cJSON *db = read_jobs(dir);
    cJSON *jobs = db ? cJSON_GetObjectItem(db, "jobs") : NULL;
    CHECK(jobs && cJSON_GetArraySize(jobs) == 0, "the fired one-shot job is gone");
    if (db)
        cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* A recurring ("every") job that is already due: firing it must ADVANCE next_run. */
static void test_due_every_job_advances(void)
{
    TEST("crontab service: a due recurring job fires and its next_run advances");
    const char *dir = "/tmp/test_crontab_service_b";
    struct edc_config *cfg = make_config(dir);
    long long past = (long long)time(NULL) - 10;
    char json[512];
    snprintf(json, sizeof(json),
             "{\"jobs\":[{\"job_id\":\"j1\",\"name\":\"n\",\"message\":\"m\","
             "\"schedule\":{\"kind\":\"every\",\"tz\":\"\",\"once\":false,\"every_seconds\":3600,"
             "\"next_run_at_unix_ts\":%lld}}]}",
             past);
    write_jobs(dir, json);

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_toolbox *tb = edc_toolbox_new(cfg, edc_scheduler_get_actor(sched), NULL);

    edc_services_pump(sched, 1000);

    cJSON *db = read_jobs(dir);
    cJSON *jobs = db ? cJSON_GetObjectItem(db, "jobs") : NULL;
    cJSON *job = jobs ? cJSON_GetArrayItem(jobs, 0) : NULL;
    cJSON *schedule = job ? cJSON_GetObjectItem(job, "schedule") : NULL;
    cJSON *next = schedule ? cJSON_GetObjectItem(schedule, "next_run_at_unix_ts") : NULL;
    CHECK(jobs && cJSON_GetArraySize(jobs) == 1, "the recurring job is kept");
    CHECK(next && cJSON_IsNumber(next) && (time_t)next->valuedouble > time(NULL) - 5,
          "next_run advanced into the future");
    if (db)
        cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* A not-yet-due job must be left alone by a scan that does not fire it (regression:
 * advancing every job on every scan would push a long-interval job forward forever). */
static void test_future_job_not_disturbed(void)
{
    TEST("crontab service: an idle scan does not advance a not-yet-due job");
    const char *dir = "/tmp/test_crontab_service_c";
    struct edc_config *cfg = make_config(dir);
    long long future = (long long)time(NULL) + 100000;
    char json[512];
    snprintf(json, sizeof(json),
             "{\"jobs\":[{\"job_id\":\"j1\",\"name\":\"n\",\"message\":\"m\","
             "\"schedule\":{\"kind\":\"every\",\"tz\":\"\",\"once\":false,\"every_seconds\":10,"
             "\"next_run_at_unix_ts\":%lld}}]}",
             future);
    write_jobs(dir, json);

    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    struct edc_toolbox *tb = edc_toolbox_new(cfg, edc_scheduler_get_actor(sched), NULL);

    edc_services_pump(sched, 100000000); /* force the service to run its scan */

    cJSON *db = read_jobs(dir);
    cJSON *jobs = db ? cJSON_GetObjectItem(db, "jobs") : NULL;
    cJSON *job = jobs ? cJSON_GetArrayItem(jobs, 0) : NULL;
    cJSON *schedule = job ? cJSON_GetObjectItem(job, "schedule") : NULL;
    cJSON *next = schedule ? cJSON_GetObjectItem(schedule, "next_run_at_unix_ts") : NULL;
    CHECK(jobs && cJSON_GetArraySize(jobs) == 1, "the job is kept");
    CHECK(next && cJSON_IsNumber(next) && (long long)next->valuedouble == future, "next_run is unchanged");
    if (db)
        cJSON_Delete(db);

    edc_toolbox_free(tb);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace(); /* disposable store for anything that writes state */
    test_due_once_job_fires_and_is_removed();
    test_due_every_job_advances();
    test_future_job_not_disturbed();

    log_info("  ====== %d passed, %d failed ======", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
