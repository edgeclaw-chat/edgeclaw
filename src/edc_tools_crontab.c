#include "sds.h"
#include "edc_error.h"
#include "edc_tools_crontab.h"
#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_toolbox.h"
#include "edc_tools.h"
#include "ccronexpr.h"
#include "log.h"
#include <edc_param_types.h>
#include <stdio.h>

struct edc_tool_crontab_state
{
    uv_timer_t timer;
    struct edc_tool *tool;
    uint64_t timeout_at;
    sds job_id;
    sds job_name;
    sds message;
    sds workspace;
    bool once;
    edc_actor_t *scheduler;
    struct edc_channel *channel;
};

static struct edc_tool_crontab_state *edc_tool_crontab_state_new(uint64_t timeout_at, sds job_id,
                                                                sds job_name, sds message, sds workspace,
                                                                bool once, edc_actor_t *scheduler,
                                                                struct edc_channel *channel, struct edc_tool *tool)
{
    struct edc_tool_crontab_state *state = calloc(1, sizeof(struct edc_tool_crontab_state));
    state->tool = tool;
    state->timeout_at = timeout_at;
    state->job_id = job_id;
    state->job_name = job_name;
    state->message = message;
    state->once = once;
    state->scheduler = scheduler;
    state->channel = channel;
    state->workspace = workspace;
    return state;
}

static void on_crontab_timer_close(uv_handle_t *handle)
{
    struct edc_tool_crontab_state *state = container_of(handle, struct edc_tool_crontab_state, timer);
    if (state->job_id)
        sdsfree(state->job_id);
    if (state->job_name)
        sdsfree(state->job_name);
    if (state->message)
        sdsfree(state->message);
    if (state->channel)
        edc_channel_free(state->channel);
    if (state->workspace)
        sdsfree(state->workspace);
    free(state);
}

static void edc_tool_crontab_state_free(struct edc_tool_crontab_state *state)
{
    if (!state)
        return;
    uv_timer_stop(&state->timer);
    uv_close((uv_handle_t *)&state->timer, on_crontab_timer_close);
}

static cJSON *edc_tool_crontab_load_jobs_db(const char *workspace, EdcError **error)
{
    char *jobs_path = g_build_filename(workspace, "jobs.json", NULL);

    cJSON *db = NULL;
    char *content = NULL;
    size_t len = 0;

    if (g_file_test(jobs_path, G_FILE_TEST_IS_REGULAR))
    {
        GError *gerror = NULL;
        if (!g_file_get_contents(jobs_path, &content, &len, &gerror))
        {
            edc_error_set(error, EDC_ERR_FAILED, "%s", gerror->message);
            g_error_free(gerror);
            goto out;
        }

        db = cJSON_ParseWithLength(content, len);
        free(content);
    }

out:
    free(jobs_path);
    return db;
}

static bool edc_tool_crontab_save_jobs_db(const char *workspace, cJSON *db, EdcError **error)
{
    char *jobs_path = g_build_filename(workspace, "jobs.json", NULL);
    FILE *fout = fopen(jobs_path, "w+");

    if (!fout)
    {
        const char *s = g_strerror(errno);
        edc_error_set(error, EDC_ERR_INVAL, "open %s for writting failed: %s", jobs_path, s);
        free(jobs_path);
        return FALSE;
    }
    free(jobs_path);

    char *tmp = cJSON_PrintUnformatted(db);
    fprintf(fout, "%s", tmp);
    fflush(fout);
    fclose(fout);
    cJSON_free(tmp);

    return TRUE;
}

static void edc_tool_crontab_remove_job(cJSON *jobs, char *job_id)
{
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
                                            void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{
    EdcError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(ctx->tb->conf->workspace, &error);
    if (error)
    {
        callback(ctx, NULL, error);
    }

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");

    sds result = sdsempty();
    for (int i = 0; i < cJSON_GetArraySize(jobs); ++i)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        result = sdscatprintf(result, "job name: %s\tjob id: %s",
                               cJSON_GetStringValue(cJSON_GetObjectItem(job, "name")),
                               cJSON_GetStringValue(cJSON_GetObjectItem(job, "job_id")));
    }

    cJSON_Delete(db);
    callback(ctx, result, NULL);
}

static void edc_tool_crontab_exec_remove_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                              void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
{
    cJSON *job_id = cJSON_GetObjectItem(args, "job_id");
    if (!job_id || !cJSON_IsString(job_id))
    {
        char *tmp = cJSON_PrintUnformatted(args);
        EdcError *error = edc_error_new(EDC_ERR_INVAL,
                                    "crontab tool: remove job failed. job_id is not provided in args: %s", tmp);
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

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");
    edc_tool_crontab_remove_job(jobs, cJSON_GetStringValue(job_id));

    if (!edc_tool_crontab_save_jobs_db(ctx->tb->conf->workspace, db, &error))
    {
        callback(ctx, NULL, error);
    }
    else
    {
        sds result = sdsempty();
        result = sdscatprintf(result, "Remove job %s done. (exit code: 0)", cJSON_GetStringValue(job_id));
        callback(ctx, result, NULL);
    }

    cJSON_Delete(db);
}
static sds edc_tool_crontab_build_task_message(char *job_name, char *job_message)
{
    return sdscatprintf(sdsempty(),
        "[Scheduled Task] Timer finished.\n\n"
        "Task '%s' has been triggered.\n"
        "Scheduled instruction: %s", job_name, job_message);
}

static void edc_tool_crontab_schedule(const char *workspace, edc_actor_t *scheduler, struct edc_tool *tool, cJSON *jobs);

static void edc_tool_crontab_caculate_next_run(cJSON *jobs)
{
    for (int i = 0; i < cJSON_GetArraySize(jobs); ++i)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        cJSON *job_id = cJSON_GetObjectItem(job, "job_id");
        cJSON *schedule = cJSON_GetObjectItem(job, "schedule");
        cJSON *tz_id = cJSON_GetObjectItem(schedule, "tz");
        cJSON *kind = cJSON_GetObjectItem(schedule, "kind");
        GTimeZone *tz = g_time_zone_new_identifier(cJSON_GetStringValue(tz_id));
        if (strcmp(cJSON_GetStringValue(kind), "at") == 0)
        {
        }
        else if (strcmp(cJSON_GetStringValue(kind), "every") == 0)
        {
            cJSON *interval = cJSON_GetObjectItem(schedule, "every_seconds");
            int every_seconds = interval->valueint;
            GDateTime *dt = g_date_time_new_now(tz);
            int64_t now = g_date_time_to_unix(dt);
            g_date_time_unref(dt);
            cJSON_ReplaceItemInObject(schedule, "next_run_at_unix_ts", cJSON_CreateNumber(now + every_seconds));
        }
        else if (strcmp(cJSON_GetStringValue(kind), "cron") == 0)
        {
            cJSON *expr = cJSON_GetObjectItem(schedule, "cron_expr");
            const char *error = NULL;
            cron_expr cexpr;
            cron_parse_expr(cJSON_GetStringValue(expr), &cexpr, &error);
            if (error)
            {
                log_error("job id: %s parse crontab expr failed: %s", cJSON_GetStringValue(job_id), error);
            }
            else
            {
                int interval = g_time_zone_find_interval(tz, G_TIME_TYPE_STANDARD, g_get_real_time() / 1000000);
                int32_t tz_offset = g_time_zone_get_offset(tz, interval);
                time_t next = cron_next(&cexpr, time(NULL) + tz_offset);
                next -= tz_offset;
                cJSON_ReplaceItemInObject(schedule, "next_run_at_unix_ts", cJSON_CreateNumber(next));
            }
        }

        g_time_zone_unref(tz);
    }
}

static void edc_tool_crontab_on_timeout(uv_timer_t *timer)
{
    struct edc_tool_crontab_state *state = container_of(timer, struct edc_tool_crontab_state, timer);
    struct edc_tool *tool = state->tool;
    tool->priv = NULL;

    struct edc_channel_message *message = edc_channel_message_new(state->channel, state->message);
    state->message = NULL;
    state->channel = NULL;

    edc_actor_send(state->scheduler, edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, NULL, message,
                                                     (edc_actor_data_free_fn)edc_channel_message_free));
    EdcError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(state->workspace, &error);
    if (error)
    {
        log_error("failed at load jobs db: %s", error->message);
        edc_error_free(error);
    }

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");

    if (state->once)
    {
        edc_tool_crontab_remove_job(jobs, state->job_id);
    }

    edc_tool_crontab_caculate_next_run(jobs);

    if (!edc_tool_crontab_save_jobs_db(state->workspace, db, &error))
    {
        log_error("failed at load jobs db: %s", error->message);
        edc_error_free(error);
    }

    edc_tool_crontab_schedule(state->workspace, state->scheduler, tool, jobs);
    edc_tool_crontab_state_free(state);

    cJSON_Delete(db);
}

static void edc_tool_crontab_update_task(const char *workspace, struct edc_channel *call_from, edc_actor_t *scheduler,
                                        struct edc_tool *tool, char *job_id, char *job_name, char *job_message,
                                        int64_t run_at, bool once)
{
    {
        struct edc_tool_crontab_state *old_state = tool->priv;
        edc_tool_crontab_state_free(old_state);
        tool->priv = NULL;
    }

    GDateTime *now = g_date_time_new_now_utc();
    int64_t ts = g_date_time_to_unix(now);
    if (run_at < ts)
    {
        run_at = ts + 1;
    }

    int interval = run_at - ts;
    g_date_time_unref(now);

    struct edc_tool_crontab_state *state =
        edc_tool_crontab_state_new(run_at, sdsnew(job_id), sdsnew(job_name),
                                   edc_tool_crontab_build_task_message(job_name, job_message),
                                   sdsnew(workspace), once, scheduler, call_from, tool);

    uv_timer_init(scheduler->loop, &state->timer);
    uv_timer_start(&state->timer, edc_tool_crontab_on_timeout, (uint64_t)interval * 1000, 0);
    tool->priv = state;

    GDateTime *point = g_date_time_new_from_unix_local(run_at);
    char *s = g_date_time_format_iso8601(point);
    log_info("crontabl tool schedule job name = %s, id = %s, run at %s", job_name, job_id, s);
    free(s);
    g_date_time_unref(point);
}

static void edc_tool_crontab_schedule(const char *workspace, edc_actor_t *scheduler, struct edc_tool *tool, cJSON *jobs)
{
    int n_jobs = cJSON_GetArraySize(jobs);
    if (n_jobs == 0)
    {
        return;
    }

    cJSON *job0 = cJSON_GetArrayItem(jobs, 0);
    cJSON *schedule = cJSON_GetObjectItem(job0, "schedule");
    cJSON *channel = cJSON_GetObjectItem(job0, "channel");

    bool once = cJSON_IsTrue(cJSON_GetObjectItem(schedule, "once"));
    int64_t next_time = (int64_t)cJSON_GetObjectItem(schedule, "next_run_at_unix_ts")->valuedouble;
    char *job_id = cJSON_GetStringValue(cJSON_GetObjectItem(job0, "job_id"));
    char *job_name = cJSON_GetStringValue(cJSON_GetObjectItem(job0, "name"));
    char *job_message = cJSON_GetStringValue(cJSON_GetObjectItem(job0, "message"));

    for (int i = 1; i < n_jobs; ++i)
    {
        cJSON *job_i = cJSON_GetArrayItem(jobs, i);
        cJSON *schedule_i = cJSON_GetObjectItem(job_i, "schedule");
        int64_t next_time_i = (int64_t)(cJSON_GetObjectItem(schedule_i, "next_run_at_unix_ts")->valuedouble);
        bool once_i = cJSON_IsTrue(cJSON_GetObjectItem(schedule, "once"));
        char *job_name_i = cJSON_GetStringValue(cJSON_GetObjectItem(job_i, "name"));
        cJSON *channel_i = cJSON_GetObjectItem(job_i, "channel");

        if (next_time_i < next_time)
        {
            job_id = cJSON_GetStringValue(cJSON_GetObjectItem(job_i, "job_id"));
            job_name = job_name_i;
            job_message = cJSON_GetStringValue(cJSON_GetObjectItem(job_i, "message"));
            next_time = next_time_i;
            once = once_i;
            channel = channel_i;
        }
    }

    struct edc_tool_crontab_state *state = tool->priv;
    if (!state || (next_time < state->timeout_at && !strcmp(job_id, state->job_id) == 0))
    {
        edc_tool_crontab_update_task(workspace, edc_channel_from_json(channel), scheduler, tool, job_id, job_name,
                                    job_message, next_time, once);
    }
}

static void edc_tool_crontab_exec_add_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                           void (*callback)(struct edc_toolbox_call_ctx *ctx, sds , EdcError *))
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
        EdcError *error = edc_error_new(EDC_ERR_INVAL,
                                    "crontab tool: message is required for action add, args = %s", tmp);
        cJSON_free(tmp);
        callback(ctx, NULL, error);
        return;
    }

    if (!at && !every_seconds && !cexpr)
    {
        char *tmp = cJSON_PrintUnformatted(args);
        EdcError *error =
            edc_error_new(EDC_ERR_INVAL,
                        "'at' / 'every_seconds' / 'cron_expr' should be provided for schedule. args = %s", tmp);
        callback(ctx, NULL, error);
        cJSON_free(tmp);
        return;
    }

    GTimeZone *g_tz = NULL;
    if (tz)
    {
        g_tz = g_time_zone_new_identifier(cJSON_GetStringValue(tz));
        if (!g_tz)
        {
            char *tmp = cJSON_PrintUnformatted(tz);
            EdcError *error = edc_error_new(EDC_ERR_INVAL, "timezone is invalid: %s", tmp);
            cJSON_free(tmp);
            callback(ctx, NULL, error);
            return;
        }
    }
    else
    {
        g_tz = g_time_zone_new_local();
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
        char *s = g_strndup(cJSON_GetStringValue(message), 30);
        cJSON_AddStringToObject(job, "name", s);
        free(s);
    }

    char *job_id = g_uuid_string_random();
    cJSON_AddStringToObject(job, "job_id", job_id);
    free(job_id);

    cJSON_AddItemToObject(job, "channel", edc_channel_to_json(ctx->call_from));

    cJSON *schedule = cJSON_AddObjectToObject(job, "schedule");
    cJSON_AddStringToObject(schedule, "tz", g_time_zone_get_identifier(g_tz));

    if (at)
    {
        cJSON_AddStringToObject(schedule, "kind", "at");
        GDateTime *dt = g_date_time_new_from_iso8601(cJSON_GetStringValue(at), g_tz);
        if (!dt)
        {
            char *tmp = cJSON_PrintUnformatted(at);
            EdcError *error =
                edc_error_new(EDC_ERR_INVAL, "'at' is not iso8601 format: %s", tmp);
            callback(ctx, NULL, error);
            cJSON_free(tmp);
            g_time_zone_unref(g_tz);
            return;
        }

        cJSON_AddNumberToObject(schedule, "next_run_at_unix_ts", g_date_time_to_unix(dt));
        cJSON_AddBoolToObject(schedule, "once", TRUE);
        g_date_time_unref(dt);
    }
    else if (every_seconds)
    {
        cJSON_AddStringToObject(schedule, "kind", "every");
        cJSON_AddNumberToObject(schedule, "every_seconds", every_seconds->valueint);
        GDateTime *dt = g_date_time_new_now_local();
        int64_t now = g_date_time_to_unix(dt);
        g_date_time_unref(dt);

        cJSON_AddNumberToObject(schedule, "next_run_at_unix_ts", now + every_seconds->valueint);
        cJSON_AddBoolToObject(schedule, "once", FALSE);
    }
    else if (cexpr)
    {
        cJSON_AddStringToObject(schedule, "kind", "cron");
        cJSON_AddStringToObject(schedule, "cron_expr", cJSON_GetStringValue(cexpr));

        cron_expr expr;
        const char *error = NULL;
        // 解析表达式
        cron_parse_expr(cJSON_GetStringValue(cexpr), &expr, &error);

        if (error)
        {
            EdcError *e = edc_error_new(EDC_ERR_INVAL, "parse cron expr %s failed: %s",
                                    cJSON_GetStringValue(cexpr), error);
            callback(ctx, NULL, e);

            g_time_zone_unref(g_tz);
            return;
        }

        int interval = g_time_zone_find_interval(g_tz, G_TIME_TYPE_STANDARD, g_get_real_time() / 1000000);
        int32_t tz_offset = g_time_zone_get_offset(g_tz, interval);

        time_t next = cron_next(&expr, time(NULL) + tz_offset);
        next -= tz_offset;
        cJSON_AddNumberToObject(schedule, "next_run_at_unix_ts", next);
        cJSON_AddBoolToObject(schedule, "once", FALSE);
    }

    cJSON_AddStringToObject(job, "message", cJSON_GetStringValue(message));
    cJSON_AddItemToArray(jobs, job);
    edc_tool_crontab_schedule(conf->workspace, tb->scheduler, tool, jobs);

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
        callback(ctx, result, NULL);
    }

    g_time_zone_unref(g_tz);
    cJSON_Delete(db);
}

static void edc_tool_crontab_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                       void (*callback)(struct edc_toolbox_call_ctx *, sds , EdcError *))
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
    LL_APPEND(params, edc_tool_param_new("action", edc_param_str_type("Action to perform", "add", "list", "remove", NULL), TRUE));
    LL_APPEND(params, edc_tool_param_new("message", edc_param_str_type("Reminder message (for add)", NULL), FALSE));

    LL_APPEND(params, edc_tool_param_new("every_seconds", edc_param_int_type("Interval in seconds (for recurring task)", 1, G_MAXINT64), FALSE));
    LL_APPEND(params, edc_tool_param_new("cron_expr", edc_param_str_type("Cron expression like '0 2 * * *' (for scheduled tasks)", NULL), FALSE));

    LL_APPEND(params, edc_tool_param_new("tz", edc_param_str_type("IANA timezone for cron expressions (e.g. 'China/Beijing')", NULL), FALSE));

    LL_APPEND(params, edc_tool_param_new("at", edc_param_str_type("ISO datetime for one-time execution (e.g. '2026-05-01T12:00:00')", NULL), FALSE));

    LL_APPEND(params, edc_tool_param_new("job_id", edc_param_str_type("Job ID (for remove)", NULL), FALSE));
    LL_APPEND(params, edc_tool_param_new("name", edc_param_str_type("Job Name (for add)", NULL), FALSE));

    struct edc_tool *tool = edc_tool_new("crontab", "Schedule reminders and recurring tasks. Actions: add, list, remove.",
                                       params, edc_tool_crontab_exec_async);

    tool->priv = NULL;
    tool->priv_free_fn = (EdcDestroyNotify)edc_tool_crontab_state_free;

    edc_toolbox_register(tb, tool);

    EdcError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(tb->conf->workspace, &error);
    if (!db)
    {
        return;
    }

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");
    edc_tool_crontab_schedule(tb->conf->workspace, tb->scheduler, tool, jobs);
    cJSON_Delete(db);
}

void edc_tools_crontab_register(struct edc_toolbox *tb)
{
    edc_tool_create_crontab(tb);
}
