#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "edc_config.h"
#include "edc_tools_crontab.h"
#include "ccronexpr.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_crontab_jobs.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_service.h"
#include "edc_string_utils.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "log.h"
#include "sds.h"
#include <edc_param_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "timezone.h"

/* ---------- wall-clock <-> unix time, for schedules WITHOUT an IANA tz ----------
 * A user writing at="2026-10-18T15:00:00" (no tz) means 15:00 on THIS host, i.e.
 * host-local time. timegm() would read that wall clock as UTC and silently shift
 * every such job by the host's UTC offset (+8h on Asia/Shanghai), so schedules
 * fired hours late (or, for a past-looking local time, hours early). mktime()
 * honours /etc/localtime (and TZ) for the local->unix direction.
 * For the cron direction ccronexpr parses a timestamp with gmtime_r(), so the
 * local wall clock is passed in re-encoded as a pseudo-UTC timestamp. */

/* The host-local / UTC conversions below use edc_localtime_r() / edc_gmtime_r() from
 * edc_compat.h: the two platforms disagree on the shape of the libc call, and that is the
 * compat layer's business, not this module's. */

/* host-local wall clock -> real unix time (tm_isdst left to the host zone) */
static time_t edc_crontab_local_wallclock_to_unix(struct tm *tm)
{
    tm->tm_isdst = -1;
    return mktime(tm);
}

/* real unix time -> host-local wall clock re-encoded as a pseudo-unix timestamp */
static time_t edc_crontab_unix_to_local_wallclock(time_t unix_ts)
{
    struct tm tm = {0};
    if (!edc_localtime_r(&unix_ts, &tm))
        return (time_t)-1;
    return timegm(&tm);
}

/* pseudo-unix timestamp (local wall clock) -> real unix time */
static time_t edc_crontab_local_wallclock_to_unix_ts(time_t wall_ts)
{
    struct tm tm = {0};
    if (!edc_gmtime_r(&wall_ts, &tm))
        return (time_t)-1;
    return edc_crontab_local_wallclock_to_unix(&tm);
}

/* next cron occurrence for an expression written in the HOST's local time */
static time_t edc_crontab_cron_next_local(cron_expr *expr)
{
    time_t now = time(NULL);
    time_t local_now = edc_crontab_unix_to_local_wallclock(now);
    if (local_now < 0)
        return now + 60;
    time_t next = edc_crontab_local_wallclock_to_unix_ts(cron_next(expr, local_now));
    return next < 0 ? now + 60 : next;
}

static char *edc_uuid_gen(void)
{
    unsigned char b[16];
    for (int i = 0; i < 16; i++)
        b[i] = (unsigned char)(rand() % 256);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    char *s = malloc(37);
    snprintf(s, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

/* The crontab schedules are DATA (workspace/jobs.json); the ticking is a background
 * SERVICE (edc_service.h) -- ONE shared process-wide timer, not a uv_timer per tool.
 * This state is owned by the tool (tool->priv) and freed with it. */
struct edc_tool_crontab_svc
{
    struct edc_scheduler *sched;  /* service registry owner */
    edc_actor_t *scheduler_actor; /* where a fired job's message is injected */
    sds workspace;
    struct edc_service *service;
};

static void edc_tool_crontab_state_free(struct edc_tool_crontab_svc *state)
{
    if (!state)
        return;
    if (state->workspace)
        sdsfree(state->workspace);
    free(state);
}

static void edc_tool_crontab_reschedule(struct edc_tool_crontab_svc *state, cJSON *jobs);
static void edc_tool_crontab_scan(struct edc_service *svc, void *ud);

static cJSON *edc_tool_crontab_load_jobs_db(const char *workspace, EdcError **error)
{
    sds jobs_path = edc_path_join(workspace, "jobs.json", NULL);

    cJSON *db = NULL;

    if (edc_file_is_regular(jobs_path))
    {
        EdcError *gerror = NULL;
        sds content = edc_file_read(jobs_path, &gerror);
        if (!content)
        {
            edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
            edc_error_free(gerror);
            goto out;
        }

        db = cJSON_Parse(content);
        sdsfree(content);
    }

out:
    sdsfree(jobs_path);
    return db;
}

static bool edc_tool_crontab_save_jobs_db(const char *workspace, cJSON *db, EdcError **error)
{
    sds jobs_path = edc_path_join(workspace, "jobs.json", NULL);
    /* jobs.json lives in the model-writable workspace: refuse to follow a
     * symlink (or write outside the workspace) so it cannot be used to redirect
     * this unsandboxed write into a protected path. */
    FILE *fout = edc_file_fopen_safe(jobs_path, "w+", workspace);

    if (!fout)
    {
        const char *s = strerror(errno);
        edc_error_set(error, EDC_ERR_INVAL, "open %s for writing failed: %s", jobs_path, s);
        sdsfree(jobs_path);
        return false;
    }
    sdsfree(jobs_path);

    char *tmp = cJSON_PrintUnformatted(db);
    fprintf(fout, "%s", tmp);
    fflush(fout);
    fclose(fout);
    cJSON_free(tmp);

    return true;
}

static void edc_tool_crontab_remove_job(cJSON *jobs, char *job_id)
{
    if (!jobs)
        return;

    int target = -1;
    for (int i = 0; i < cJSON_GetArraySize(jobs); ++i)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        cJSON *job_id_i = cJSON_GetObjectItem(job, "job_id");
        if (strcmp(cJSON_GetStringValue(job_id_i), job_id) == 0)
        {
            target = i;
            break;
        }
    }

    if (target < 0)
    {
        log_error("job %s is not in jobs db.", job_id);
    }
    else
    {
        cJSON_DeleteItemFromArray(jobs, target);
    }
}

static void edc_tool_crontab_exec_list_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                             void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    EdcError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(ctx->tb->conf->workspace, &error);
    if (error)
    {
        callback(ctx, NULL, error);
        return;
    }

    if (!db)
    {
        callback(ctx, sdsempty(), NULL);
        return;
    }

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");
    if (!jobs)
    {
        cJSON_Delete(db);
        callback(ctx, sdsempty(), NULL);
        return;
    }

    sds result = sdsempty();
    for (int i = 0; i < cJSON_GetArraySize(jobs); ++i)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        result =
            sdscatprintf(result, "job name: %s\tjob id: %s", cJSON_GetStringValue(cJSON_GetObjectItem(job, "name")),
                         cJSON_GetStringValue(cJSON_GetObjectItem(job, "job_id")));
    }

    cJSON_Delete(db);
    edc_toolbox_emit_tool_message_plain(ctx, "crontab list", result);
    callback(ctx, result, NULL);
}

static void edc_tool_crontab_exec_remove_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                               void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    cJSON *job_id = cJSON_GetObjectItem(args, "job_id");
    if (!job_id || !cJSON_IsString(job_id))
    {
        char *tmp = cJSON_PrintUnformatted(args);
        EdcError *error =
            edc_error_new(EDC_ERR_INVAL, "crontab tool: remove job failed. job_id is not provided in args: %s", tmp);
        cJSON_free(tmp);
        callback(ctx, NULL, error);
        return;
    }

    EdcError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(ctx->tb->conf->workspace, &error);
    if (error)
    {
        callback(ctx, NULL, error);
        return;
    }

    if (!db)
    {
        EdcError *err = edc_error_new(EDC_ERR_INVAL, "crontab: no jobs database found, job %s does not exist",
                                      cJSON_GetStringValue(job_id));
        callback(ctx, NULL, err);
        return;
    }

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");
    edc_tool_crontab_remove_job(jobs, cJSON_GetStringValue(job_id));

    if (!edc_tool_crontab_save_jobs_db(ctx->tb->conf->workspace, db, &error))
    {
        callback(ctx, NULL, error);
    }
    else
    {
        /* A job is gone: re-arm for whatever is next (or sleep long when empty). */
        edc_tool_crontab_reschedule(tool->priv, jobs);
        sds result = sdsempty();
        result = sdscatprintf(result, "Remove job %s done. (exit code: 0)", cJSON_GetStringValue(job_id));
        sds header = sdscatprintf(sdsempty(), "crontab remove %s", cJSON_GetStringValue(job_id));
        edc_toolbox_emit_tool_message_plain(ctx, header, result);
        sdsfree(header);
        callback(ctx, result, NULL);
    }

    cJSON_Delete(db);
}
/* Advance ONE job's next_run after it FIRED: "every" -> now + interval, "cron" -> the
 * next occurrence, "at" -> nothing (a one-shot is removed by the caller). Only fired
 * jobs are advanced: advancing every job on every scan would push a not-yet-due job
 * forward each time, so a long-interval job would never come due. */
static void edc_tool_crontab_advance_job(cJSON *job)
{
    cJSON *schedule = cJSON_GetObjectItem(job, "schedule");
    cJSON *kind = schedule ? cJSON_GetObjectItem(schedule, "kind") : NULL;
    const char *k = cJSON_GetStringValue(kind);
    if (!k)
        return;

    if (strcmp(k, "every") == 0)
    {
        cJSON *interval = cJSON_GetObjectItem(schedule, "every_seconds");
        int every_seconds = interval ? interval->valueint : 60;
        cJSON_ReplaceItemInObject(schedule, "next_run_at_unix_ts",
                                  cJSON_CreateNumber((int64_t)time(NULL) + every_seconds));
    }
    else if (strcmp(k, "cron") == 0)
    {
        cJSON *expr = cJSON_GetObjectItem(schedule, "cron_expr");
        const char *error = NULL;
        cron_expr cexpr;
        cron_parse_expr(cJSON_GetStringValue(expr), &cexpr, &error);
        if (error)
        {
            log_error("crontab: parse cron expr failed: %s", error);
            return;
        }
        const char *tz_name = cJSON_GetStringValue(cJSON_GetObjectItem(schedule, "tz"));
        time_t next;
        if (tz_name && tz_name[0])
        {
            time_t local_now = timezone_local_time(tz_name, time(NULL));
            time_t local_next = cron_next(&cexpr, local_now);
            next = timezone_gmt_time(tz_name, local_next);
            if (next < 0)
                next = time(NULL) + 60;
        }
        else
        {
            next = edc_crontab_cron_next_local(&cexpr);
        }
        cJSON_ReplaceItemInObject(schedule, "next_run_at_unix_ts", cJSON_CreateNumber(next));
    }
}

/* Milliseconds until the soonest next_run across all jobs (never below 1s: a 0 or
 * negative delay would be a busy loop), or -1 when there are no jobs. */
static int64_t edc_tool_crontab_next_delta_ms(cJSON *jobs, time_t now)
{
    int64_t best = 0;
    bool any = false;
    int n = cJSON_GetArraySize(jobs);
    for (int i = 0; i < n; ++i)
    {
        cJSON *schedule = cJSON_GetObjectItem(cJSON_GetArrayItem(jobs, i), "schedule");
        cJSON *ts = schedule ? cJSON_GetObjectItem(schedule, "next_run_at_unix_ts") : NULL;
        if (!ts || !cJSON_IsNumber(ts))
            continue;
        int64_t delta = (int64_t)ts->valuedouble - (int64_t)now;
        if (!any || delta < best)
            best = delta;
        any = true;
    }
    if (!any)
        return -1; /* no jobs: the caller sleeps long */
    if (best < 1)
        best = 1; /* due now or overdue: fire on the next (≥1s) tick */
    return best;
}

/* Re-arm the shared service for the next due job (or a long sleep when idle; a later
 * add/remove re-arms it sooner). */
static void edc_tool_crontab_reschedule(struct edc_tool_crontab_svc *state, cJSON *jobs)
{
    if (!state || !state->sched || !state->service)
        return;
    int64_t delta_s = edc_tool_crontab_next_delta_ms(jobs, time(NULL));
    uint64_t iv = (delta_s < 0) ? (uint64_t)3600 * 1000 : (uint64_t)delta_s * 1000;
    edc_service_set_interval(state->sched, state->service, iv);
}

/* The service body: fire every due job (inject its reminder message), drop the
 * one-shots, recompute the recurrences, persist, and re-arm for the next one. Runs in
 * the scheduler handler (a background service never runs from the timer callback). */
static void edc_tool_crontab_scan(struct edc_service *svc, void *ud)
{
    (void)svc;
    struct edc_tool_crontab_svc *state = ud;
    EdcError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(state->workspace, &error);
    if (!db)
    {
        if (error)
            edc_error_free(error);
        return;
    }
    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");
    if (!jobs)
    {
        cJSON_Delete(db);
        return;
    }

    time_t now = time(NULL);
    int n = cJSON_GetArraySize(jobs);
    for (int i = 0; i < n;)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        cJSON *schedule = cJSON_GetObjectItem(job, "schedule");
        cJSON *ts = schedule ? cJSON_GetObjectItem(schedule, "next_run_at_unix_ts") : NULL;
        if (!ts || !cJSON_IsNumber(ts) || (time_t)ts->valuedouble > now)
        {
            i++;
            continue;
        }
        const char *job_name = cJSON_GetStringValue(cJSON_GetObjectItem(job, "name"));
        const char *job_msg = cJSON_GetStringValue(cJSON_GetObjectItem(job, "message"));
        bool once = cJSON_IsTrue(cJSON_GetObjectItem(schedule, "once"));
        sds text = edc_crontab_task_message(job_name, job_msg);
        struct edc_channel_message *message =
            edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, text);
        edc_actor_send(state->scheduler_actor,
                       edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, NULL, edc_channel_input_new(message),
                                         (edc_actor_data_free_fn)edc_channel_input_free));
        log_info("crontab fired job '%s'", job_name ? job_name : "?");
        if (once)
        {
            cJSON_DeleteItemFromArray(jobs, i);
            n--;
        }
        else
        {
            edc_tool_crontab_advance_job(job); /* only the job that fired */
            i++;
        }
    }

    if (!edc_tool_crontab_save_jobs_db(state->workspace, db, &error))
    {
        log_error("crontab: failed to save jobs db: %s", error && error->message ? error->message : "?");
        if (error)
            edc_error_free(error);
    }
    edc_tool_crontab_reschedule(state, jobs);
    cJSON_Delete(db);
}

static void edc_tool_crontab_exec_add_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                            void (*callback)(struct edc_toolbox_call_ctx *ctx, sds, EdcError *))
{
    cJSON *name = cJSON_GetObjectItem(args, "name");
    cJSON *every_seconds = cJSON_GetObjectItem(args, "every_seconds");
    cJSON *at = cJSON_GetObjectItem(args, "at");
    cJSON *cexpr = cJSON_GetObjectItem(args, "cron_expr");
    cJSON *tz = cJSON_GetObjectItem(args, "tz");
    cJSON *message = cJSON_GetObjectItem(args, "message");

    if (!message)
    {
        char *tmp = cJSON_PrintUnformatted(args);
        EdcError *error =
            edc_error_new(EDC_ERR_INVAL, "crontab tool: message is required for action add, args = %s", tmp);
        cJSON_free(tmp);
        callback(ctx, NULL, error);
        return;
    }

    if (!at && !every_seconds && !cexpr)
    {
        char *tmp = cJSON_PrintUnformatted(args);
        EdcError *error = edc_error_new(
            EDC_ERR_INVAL, "'at' / 'every_seconds' / 'cron_expr' should be provided for schedule. args = %s", tmp);
        callback(ctx, NULL, error);
        cJSON_free(tmp);
        return;
    }

    const char *tz_name = tz ? cJSON_GetStringValue(tz) : NULL;
    if (tz && (!tz_name || !find_timezone(tz_name)))
    {
        char *tmp = cJSON_PrintUnformatted(tz);
        EdcError *error = edc_error_new(EDC_ERR_INVAL, "timezone is invalid: %s", tmp);
        cJSON_free(tmp);
        callback(ctx, NULL, error);
        return;
    }

    struct edc_toolbox *tb = ctx->tb;
    struct edc_config *conf = tb->conf;

    EdcError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(conf->workspace, &error);
    cJSON *jobs = NULL;

    if (db)
    {
        jobs = cJSON_GetObjectItem(db, "jobs");
    }
    else
    {
        db = cJSON_CreateObject();
        jobs = cJSON_AddArrayToObject(db, "jobs");
    }

    cJSON *job = cJSON_CreateObject();
    if (name)
    {
        cJSON_AddStringToObject(job, "name", cJSON_GetStringValue(name));
    }
    else
    {
        sds s = edc_str_utf8_truncate(cJSON_GetStringValue(message), 30, false);
        cJSON_AddStringToObject(job, "name", s);
        sdsfree(s);
    }

    char *job_id = edc_uuid_gen();
    cJSON_AddStringToObject(job, "job_id", job_id);
    free(job_id);

    cJSON_AddItemToObject(job, "channel", edc_channel_to_json());

    cJSON *schedule = cJSON_AddObjectToObject(job, "schedule");
    cJSON_AddStringToObject(schedule, "tz", tz_name ? tz_name : "");

    if (at)
    {
        cJSON_AddStringToObject(schedule, "kind", "at");
        struct tm tm = {0};
        const char *p = cJSON_GetStringValue(at);
        bool at_ok = false;
#ifdef _WIN32
        {
            int year, mon, day, hour, min, sec;
            if (sscanf(p, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) == 6)
            {
                tm.tm_year = year - 1900;
                tm.tm_mon = mon - 1;
                tm.tm_mday = day;
                tm.tm_hour = hour;
                tm.tm_min = min;
                tm.tm_sec = sec;
                at_ok = true;
            }
        }
#else
        if (strptime(p, "%Y-%m-%dT%H:%M:%S", &tm))
            at_ok = true;
#endif
        if (!at_ok)
        {
            char *tmp = cJSON_PrintUnformatted(at);
            EdcError *e = edc_error_new(EDC_ERR_INVAL, "'at' is not iso8601 format: %s", tmp);
            callback(ctx, NULL, e);
            cJSON_free(tmp);
            cJSON_Delete(job);
            cJSON_Delete(db);
            return;
        }
        /* With an IANA tz the wall clock belongs to that zone: timegm() encodes it
         * as a pseudo-UTC timestamp and timezone_gmt_time() maps it to real UTC.
         * Without one it is host-local time, so mktime() is the correct direction. */
        time_t utc_ts;
        if (tz_name && tz_name[0])
            utc_ts = timezone_gmt_time(tz_name, timegm(&tm));
        else
            utc_ts = edc_crontab_local_wallclock_to_unix(&tm);
        if (utc_ts < 0)
        {
            cJSON_Delete(job);
            cJSON_Delete(db);
            EdcError *e = edc_error_new(EDC_ERR_INVAL, "'at' time is out of timezone database range");
            callback(ctx, NULL, e);
            return;
        }
        cJSON_AddNumberToObject(schedule, "next_run_at_unix_ts", utc_ts);
        cJSON_AddBoolToObject(schedule, "once", true);
    }
    else if (every_seconds)
    {
        cJSON_AddStringToObject(schedule, "kind", "every");
        cJSON_AddNumberToObject(schedule, "every_seconds", every_seconds->valueint);
        int64_t now = (int64_t)time(NULL);
        cJSON_AddNumberToObject(schedule, "next_run_at_unix_ts", now + every_seconds->valueint);
        cJSON_AddBoolToObject(schedule, "once", false);
    }
    else if (cexpr)
    {
        cJSON_AddStringToObject(schedule, "kind", "cron");
        cJSON_AddStringToObject(schedule, "cron_expr", cJSON_GetStringValue(cexpr));

        cron_expr expr;
        const char *cron_err = NULL;
        cron_parse_expr(cJSON_GetStringValue(cexpr), &expr, &cron_err);

        if (cron_err)
        {
            EdcError *e =
                edc_error_new(EDC_ERR_INVAL, "parse cron expr %s failed: %s", cJSON_GetStringValue(cexpr), cron_err);
            callback(ctx, NULL, e);
            cJSON_Delete(job);
            cJSON_Delete(db);
            return;
        }

        time_t next;
        if (tz_name && tz_name[0])
        {
            time_t local_now = timezone_local_time(tz_name, time(NULL));
            time_t local_next = cron_next(&expr, local_now);
            next = timezone_gmt_time(tz_name, local_next);
            if (next < 0)
                next = time(NULL) + 60;
        }
        else
        {
            next = edc_crontab_cron_next_local(&expr);
        }
        cJSON_AddNumberToObject(schedule, "next_run_at_unix_ts", next);
        cJSON_AddBoolToObject(schedule, "once", false);
    }

    cJSON_AddStringToObject(job, "message", cJSON_GetStringValue(message));
    cJSON_AddItemToArray(jobs, job);
    /* The job is in the list now: arm the service for the soonest deadline. */
    edc_tool_crontab_reschedule(tool->priv, jobs);

    if (!edc_tool_crontab_save_jobs_db(tb->conf->workspace, db, &error))
    {
        callback(ctx, NULL, error);
    }
    else
    {
        sds result = sdsempty();
        char *s = cJSON_PrintUnformatted(job);
        result = sdscatprintf(result, "Add job %s done. (exit code: 0)", s);
        cJSON_free(s);
        sds header = sdscatprintf(sdsempty(), "crontab add");
        edc_toolbox_emit_tool_message_plain(ctx, header, result);
        sdsfree(header);
        callback(ctx, result, NULL);
    }

    cJSON_Delete(db);
}

static void edc_tool_crontab_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                        void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *))
{
    char *action = cJSON_GetStringValue(cJSON_GetObjectItem(args, "action"));
    if (strcmp(action, "add") == 0)
    {
        edc_tool_crontab_exec_add_async(ctx, tool, args, callback);
    }
    else if (strcmp(action, "list") == 0)
    {
        edc_tool_crontab_exec_list_async(ctx, tool, args, callback);
    }
    else if (strcmp(action, "remove") == 0)
    {
        edc_tool_crontab_exec_remove_async(ctx, tool, args, callback);
    }
}

static void edc_tool_create_crontab(struct edc_toolbox *tb)
{
    struct edc_tool_param *params = NULL;
    LL_APPEND(params, edc_tool_param_new("action",
                                         edc_param_str_type("Action to perform", "add", "list", "remove", NULL), true));
    LL_APPEND(params, edc_tool_param_new("message", edc_param_str_type("Reminder message (for add)", NULL), false));

    LL_APPEND(params,
              edc_tool_param_new("every_seconds",
                                 edc_param_int_type("Interval in seconds (for recurring task)", 1, G_MAXINT64), false));
    LL_APPEND(params, edc_tool_param_new(
                          "cron_expr",
                          edc_param_str_type("Cron expression like '0 2 * * *' (for scheduled tasks)", NULL), false));

    LL_APPEND(params,
              edc_tool_param_new(
                  "tz", edc_param_str_type("IANA timezone for cron expressions (e.g. 'China/Beijing')", NULL), false));

    LL_APPEND(
        params,
        edc_tool_param_new(
            "at", edc_param_str_type("ISO datetime for one-time execution (e.g. '2026-05-01T12:00:00')", NULL), false));

    LL_APPEND(params, edc_tool_param_new("job_id", edc_param_str_type("Job ID (for remove)", NULL), false));
    LL_APPEND(params, edc_tool_param_new("name", edc_param_str_type("Job Name (for add)", NULL), false));

    struct edc_tool *tool =
        edc_tool_new("crontab", "Schedule reminders and recurring tasks. Actions: add, list, remove.", params,
                     edc_tool_crontab_exec_async);

    /* The schedule data stays in jobs.json; the ticking is one shared background
     * service. The service's state is owned by the tool (tool->priv). */
    struct edc_tool_crontab_svc *state = calloc(1, sizeof(*state));
    state->sched = tb->scheduler ? tb->scheduler->state : NULL;
    state->scheduler_actor = tb->scheduler;
    state->workspace = sdsnew(tb->conf->workspace);
    tool->priv = state;
    tool->priv_free_fn = (EdcDestroyNotify)edc_tool_crontab_state_free;

    edc_tool_set_snippet(tool, "Schedule a reminder or recurring job");
    edc_toolbox_register(tb, tool);

    if (state->sched)
    {
        state->service = edc_service_register(state->sched, "crontab", 3600 * 1000, edc_tool_crontab_scan, state);
        EdcError *error = NULL;
        cJSON *db = edc_tool_crontab_load_jobs_db(tb->conf->workspace, &error);
        if (db)
        {
            edc_tool_crontab_reschedule(state, cJSON_GetObjectItem(db, "jobs"));
            cJSON_Delete(db);
        }
        else if (error)
        {
            edc_error_free(error);
        }
    }
}

void edc_tools_crontab_register(struct edc_toolbox *tb)
{
    edc_tool_create_crontab(tb);
}
