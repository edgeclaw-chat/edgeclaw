/* test_state_jobs: the v15 `jobs` table and its CRUD + JSON bridge.
 *
 * The crontab tool and /jobs work on a cJSON {"jobs":[...]} document; the bridge
 * converts that to/from regular columns in edgeclaw.db. These pin down that round
 * trip (no field lost, channel kept as JSON text) and the row-level CRUD used by
 * the service (advance next_run, remove, clear).
 */
#include "test_ws.h"
#include "edc_state_store.h"
#include "sds.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: %s\n", msg);                                                                    \
            g_failed++;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("    ok: %s\n", msg);                                                                               \
            g_passed++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void reset(void)
{
    edc_test_store_reset();
}

static void test_add_list_roundtrip(void)
{
    printf("\n-- add / list round trip --\n");
    reset();

    struct edc_job_row j = {0};
    j.job_id = "11111111-2222-4333-8444-555555555555";
    j.name = "nightly build";
    j.message = "run the build";
    j.channel = "{\"type\":\"adapter\"}";
    j.kind = "cron";
    j.tz = "China/Beijing";
    j.cron_expr = "0 2 * * *";
    j.next_run_at = 1799000000;
    j.once = 0;
    CHECK(edc_state_store_job_add(&j), "add a cron job");

    struct edc_job_row *rows = NULL;
    int n = 0;
    CHECK(edc_state_store_job_list(&rows, &n) && n == 1, "list returns one job");
    if (n == 1)
    {
        CHECK(strcmp(rows[0].job_id, j.job_id) == 0, "job_id round trips");
        CHECK(strcmp(rows[0].name, "nightly build") == 0, "name round trips");
        CHECK(strcmp(rows[0].message, "run the build") == 0, "message round trips");
        CHECK(strcmp(rows[0].channel, "{\"type\":\"adapter\"}") == 0, "channel is kept as JSON text");
        CHECK(strcmp(rows[0].kind, "cron") == 0, "kind round trips");
        CHECK(strcmp(rows[0].tz, "China/Beijing") == 0, "tz round trips");
        CHECK(strcmp(rows[0].cron_expr, "0 2 * * *") == 0, "cron_expr round trips");
        CHECK(rows[0].next_run_at == 1799000000, "next_run_at round trips");
        CHECK(rows[0].once == 0, "once round trips");
    }
    edc_job_rows_free(rows, n);
}

static void test_bridge_roundtrip(void)
{
    printf("\n-- JSON bridge round trip --\n");
    reset();

    const char *doc =
        "{\"jobs\":["
        "{\"job_id\":\"aaaa\",\"name\":\"remind\",\"message\":\"drink water\","
        "\"channel\":{\"type\":\"adapter\"},"
        "\"schedule\":{\"kind\":\"every\",\"tz\":\"\",\"cron_expr\":\"\","
        "\"every_seconds\":60,\"next_run_at_unix_ts\":1000,\"once\":false}},"
        "{\"job_id\":\"bbbb\",\"name\":\"once\",\"message\":\"ping\","
        "\"channel\":null,"
        "\"schedule\":{\"kind\":\"at\",\"tz\":\"\",\"cron_expr\":\"\","
        "\"next_run_at_unix_ts\":2000,\"once\":true}}"
        "]}";
    cJSON *in = cJSON_Parse(doc);
    CHECK(in != NULL, "fixture parses");
    CHECK(edc_state_store_jobs_replace(in), "replace writes both jobs");

    cJSON *out = edc_state_store_jobs_json();
    CHECK(out != NULL, "bridge returns a document");
    if (out)
    {
        cJSON *jobs = cJSON_GetObjectItem(out, "jobs");
        CHECK(jobs && cJSON_GetArraySize(jobs) == 2, "two jobs came back");
        cJSON *a = jobs ? cJSON_GetArrayItem(jobs, 0) : NULL;
        cJSON *b = jobs ? cJSON_GetArrayItem(jobs, 1) : NULL;
        CHECK(a && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(a, "job_id")), "aaaa") == 0,
              "first job id kept");
        CHECK(b && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(b, "job_id")), "bbbb") == 0,
              "second job id kept");
        if (a)
        {
            cJSON *s = cJSON_GetObjectItem(a, "schedule");
            CHECK(s && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(s, "kind")), "every") == 0, "kind kept");
            CHECK(s && cJSON_GetObjectItem(s, "every_seconds") &&
                      cJSON_GetObjectItem(s, "every_seconds")->valueint == 60,
                  "every_seconds kept");
            CHECK(cJSON_GetObjectItem(a, "channel") && cJSON_IsObject(cJSON_GetObjectItem(a, "channel")),
                  "channel round trips as an object");
        }
        if (b)
        {
            cJSON *s = cJSON_GetObjectItem(b, "schedule");
            CHECK(s && cJSON_IsTrue(cJSON_GetObjectItem(s, "once")), "once=true kept");
        }
        cJSON_Delete(out);
    }
    cJSON_Delete(in);
}

static void test_update_remove_clear(void)
{
    printf("\n-- set_next_run / remove / clear --\n");
    reset();

    struct edc_job_row j = {0};
    j.job_id = "id-1";
    j.name = "n";
    j.message = "m";
    j.kind = "every";
    j.every_seconds = 60;
    j.next_run_at = 100;
    CHECK(edc_state_store_job_add(&j), "add job id-1");
    CHECK(edc_state_store_job_set_next_run("id-1", 500), "set next_run");
    CHECK(edc_state_store_job_list(NULL, NULL) == false, "list with NULL out is refused");

    struct edc_job_row *rows = NULL;
    int n = 0;
    edc_state_store_job_list(&rows, &n);
    CHECK(n == 1 && rows[0].next_run_at == 500, "next_run was updated");
    edc_job_rows_free(rows, n);

    CHECK(edc_state_store_job_remove("id-1"), "remove existing job");
    CHECK(!edc_state_store_job_remove("id-1"), "removing again reports no row");
    edc_state_store_job_list(&rows, &n);
    CHECK(n == 0, "table is empty after remove");
    edc_job_rows_free(rows, n);

    j.job_id = "id-2";
    edc_state_store_job_add(&j);
    j.job_id = "id-3";
    edc_state_store_job_add(&j);
    CHECK(edc_state_store_job_clear(), "clear all jobs");
    edc_state_store_job_list(&rows, &n);
    CHECK(n == 0, "table is empty after clear");
    edc_job_rows_free(rows, n);
}

/* The job CRUD guards are the API contract: a NULL store/argument is refused
 * rather than crashing or silently writing a half-row. */
static void test_guards(void)
{
    printf("\n-- NULL / malformed arguments --\n");
    reset();
    CHECK(!edc_state_store_job_add(NULL), "job_add(NULL) refused");
    struct edc_job_row no_id = {0};
    no_id.kind = "every";
    CHECK(!edc_state_store_job_add(&no_id), "job_add without job_id refused");
    struct edc_job_row no_kind = {0};
    no_kind.job_id = "x";
    CHECK(!edc_state_store_job_add(&no_kind), "job_add without kind refused");
    CHECK(!edc_state_store_job_remove(NULL), "job_remove(NULL) refused");
    CHECK(!edc_state_store_job_remove(""), "job_remove(\"\") refused");
    CHECK(!edc_state_store_job_set_next_run(NULL, 1), "set_next_run(NULL,..) refused");
    CHECK(!edc_state_store_job_set_next_run("", 1), "set_next_run(\"\") refused");
    CHECK(!edc_state_store_jobs_replace(NULL), "jobs_replace(NULL) refused");

    cJSON *no_jobs = cJSON_CreateObject();
    CHECK(!edc_state_store_jobs_replace(no_jobs), "jobs_replace without a jobs array refused");
    cJSON_Delete(no_jobs);
    cJSON *bad_jobs = cJSON_CreateObject();
    cJSON_AddStringToObject(bad_jobs, "jobs", "not an array");
    CHECK(!edc_state_store_jobs_replace(bad_jobs), "jobs_replace with non-array jobs refused");
    cJSON_Delete(bad_jobs);

    /* A malformed row (no job_id) is skipped, not added as an empty key. */
    cJSON *skip = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(skip, "jobs");
    cJSON_AddItemToArray(arr, cJSON_CreateObject());
    CHECK(edc_state_store_jobs_replace(skip), "jobs_replace skips rows without job_id");
    struct edc_job_row *rows = NULL;
    int n = 0;
    edc_state_store_job_list(&rows, &n);
    CHECK(n == 0, "the skipped row did not create a job");
    edc_job_rows_free(rows, n);
    cJSON_Delete(skip);
}

int main(void)
{
    edc_test_workspace();
    test_add_list_roundtrip();
    test_bridge_roundtrip();
    test_update_remove_clear();
    test_guards();
    edc_test_store_down(NULL);
    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
