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
    GSource *source;
    uint64_t timeout_at;
    gchar *job_id;
    gchar *job_name;
    gchar *message;
    gchar *workspace;
    gboolean once;
    edc_actor_t *scheduler;
    struct edc_channel *channel;
};

static struct edc_tool_crontab_state *edc_tool_crontab_state_new(GSource *source, uint64_t timeout_at, gchar *job_id,
                                                               gchar *job_name, gchar *message, gchar *workspace,
                                                               gboolean once, edc_actor_t *scheduler,
                                                               struct edc_channel *channel)
{
    struct edc_tool_crontab_state *state = g_new0(struct edc_tool_crontab_state, 1);
    state->source = source;
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

static void edc_tool_crontab_state_free(struct edc_tool_crontab_state *state)
{
    if (!state)
        return;
    if (state->source)
    {
        g_source_destroy(state->source);
        g_source_unref(state->source);
    }

    if (state->job_id)
    {
        g_free(state->job_id);
    }

    if (state->job_name)
    {
        g_free(state->job_name);
    }

    if (state->message)
    {
        g_free(state->message);
    }

    if (state->channel)
    {
        edc_channel_free(state->channel);
    }

    if (state->workspace)
    {
        g_free(state->workspace);
    }

    g_free(state);
}

static cJSON *edc_tool_crontab_load_jobs_db(const gchar *workspace, GError **error)
{
    gchar *jobs_path = g_build_filename(workspace, "jobs.json", NULL);

    cJSON *db = NULL;
    gchar *content = NULL;
    gsize len = 0;

    if (g_file_test(jobs_path, G_FILE_TEST_IS_REGULAR))
    {
        if (!g_file_get_contents(jobs_path, &content, &len, error))
        {
            goto out;
        }

        db = cJSON_ParseWithLength(content, len);
        g_free(content);
    }

out:
    g_free(jobs_path);
    return db;
}

static gboolean edc_tool_crontab_save_jobs_db(const gchar *workspace, cJSON *db, GError **error)
{
    gchar *jobs_path = g_build_filename(workspace, "jobs.json", NULL);
    FILE *fout = fopen(jobs_path, "w+");
    g_free(jobs_path);

    if (!fout)
    {
        const gchar *s = g_strerror(errno);
        *error =
            g_error_new(edc_tool_error_quark(), BC_TOOL_ARG_INVALID, "open %s for writting failed: %s", jobs_path, s);
        return FALSE;
    }
    else
    {
        gchar *tmp = cJSON_PrintUnformatted(db);
        fprintf(fout, "%s", tmp);
        fflush(fout);
        fclose(fout);
        g_free(tmp);
    }

    return TRUE;
}

static void edc_tool_crontab_remove_job(cJSON *jobs, gchar *job_id)
{
    int target = -1;
    for (int i = 0; i < cJSON_GetArraySize(jobs); ++i)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        cJSON *job_id_i = cJSON_GetObjectItem(job, "job_id");
        if (g_str_equal(cJSON_GetStringValue(job_id_i), job_id))
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
                                            void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    GError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(ctx->tb->conf->workspace, &error);
    if (error)
    {
        callback(ctx, NULL, error);
    }

    cJSON *jobs = cJSON_GetObjectItem(db, "jobs");

    GString *result = g_string_new(NULL);
    for (int i = 0; i < cJSON_GetArraySize(jobs); ++i)
    {
        cJSON *job = cJSON_GetArrayItem(jobs, i);
        g_string_append_printf(result, "job name: %s\tjob id: %s",
                               cJSON_GetStringValue(cJSON_GetObjectItem(job, "name")),
                               cJSON_GetStringValue(cJSON_GetObjectItem(job, "job_id")));
    }

    cJSON_Delete(db);
    callback(ctx, result, NULL);
}

static void edc_tool_crontab_exec_remove_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                              void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    cJSON *job_id = cJSON_GetObjectItem(args, "job_id");
    if (!job_id || !cJSON_IsString(job_id))
    {
        gchar *tmp = cJSON_PrintUnformatted(args);
        GError *error = g_error_new(edc_tool_error_quark(), BC_TOOL_ARG_INVALID,
                                    "crontab tool: remove job failed. job_id is not provided in args: %s", tmp);
        g_free(tmp);
        callback(ctx, NULL, error);
        return;
    }

    GError *error = NULL;
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
        GString *result = g_string_new(NULL);
        g_string_append_printf(result, "Remove job %s done. (exit code: 0)", cJSON_GetStringValue(job_id));
        callback(ctx, result, NULL);
    }

    cJSON_Delete(db);
}

static gchar *edc_tool_crontab_build_task_message(gchar *job_name, gchar *job_message)
{
    return g_strdup_printf("[Scheduled Task] Timer finished.\n\n"
                           "Task '%s' has been triggered.\n"
                           "Scheduled instruction: %s",
                           job_name, job_message);
}

static void edc_tool_crontab_schedule(const gchar *workspace, edc_actor_t *scheduler, struct edc_tool *tool, cJSON *jobs);

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
        if (g_str_equal(cJSON_GetStringValue(kind), "at"))
        {
        }
        else if (g_str_equal(cJSON_GetStringValue(kind), "every"))
        {
            cJSON *interval = cJSON_GetObjectItem(schedule, "every_seconds");
            int every_seconds = interval->valueint;
            GDateTime *dt = g_date_time_new_now(tz);
            gint64 now = g_date_time_to_unix(dt);
            g_date_time_unref(dt);
            cJSON_ReplaceItemInObject(schedule, "next_run_at_unix_ts", cJSON_CreateNumber(now + every_seconds));
        }
        else if (g_str_equal(cJSON_GetStringValue(kind), "cron"))
        {
            cJSON *expr = cJSON_GetObjectItem(schedule, "cron_expr");
            const gchar *error = NULL;
            cron_expr cexpr;
            cron_parse_expr(cJSON_GetStringValue(expr), &cexpr, &error);
            if (error)
            {
                log_error("job id: %s parse crontab expr failed: %s", cJSON_GetStringValue(job_id), error);
            }
            else
            {
                gint interval = g_time_zone_find_interval(tz, G_TIME_TYPE_STANDARD, g_get_real_time() / 1000000);
                gint32 tz_offset = g_time_zone_get_offset(tz, interval);
                time_t next = cron_next(&cexpr, time(NULL) + tz_offset);
                next -= tz_offset;
                cJSON_ReplaceItemInObject(schedule, "next_run_at_unix_ts", cJSON_CreateNumber(next));
            }
        }

        g_time_zone_unref(tz);
    }
}

static gboolean edc_tool_crontab_on_timeout(void *data)
{
    struct edc_tool *tool = data;
    struct edc_tool_crontab_state *state = tool->priv;
    tool->priv = NULL;

    struct edc_channel_message *message = edc_channel_message_new(state->channel, state->message);
    state->message = NULL;
    state->channel = NULL;

    edc_actor_send(state->scheduler, edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, NULL, message,
                                                     (edc_actor_data_free_fn)edc_channel_message_free));
    GError *error = NULL;
    cJSON *db = edc_tool_crontab_load_jobs_db(state->workspace, &error);
    if (error)
    {
        log_error("failed at load jobs db: %s", error->message);
        g_error_free(error);
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
        g_error_free(error);
    }

    edc_tool_crontab_schedule(state->workspace, state->scheduler, tool, jobs);
    edc_tool_crontab_state_free(state);

    cJSON_Delete(db);

    return G_SOURCE_REMOVE;
}

static void edc_tool_crontab_update_task(const gchar *workspace, struct edc_channel *call_from, edc_actor_t *scheduler,
                                        struct edc_tool *tool, gchar *job_id, gchar *job_name, gchar *job_message,
                                        gint64 run_at, gboolean once)
{
    struct edc_tool_crontab_state *state = tool->priv;
    edc_tool_crontab_state_free(state);

    GDateTime *now = g_date_time_new_now_utc();
    gint64 ts = g_date_time_to_unix(now);
    if (run_at < ts)
    {
        run_at = ts + 1;
    }

    gint interval = run_at - ts;
    g_date_time_unref(now);

    GSource *source = g_timeout_source_new_seconds(interval);
    tool->priv = edc_tool_crontab_state_new(source, run_at, g_strdup(job_id), g_strdup(job_name),
                                           edc_tool_crontab_build_task_message(job_name, job_message),
                                           g_strdup(workspace), once, scheduler, call_from);

    g_source_set_callback(source, edc_tool_crontab_on_timeout, tool, NULL);
    g_source_attach(source, scheduler->context);

    GDateTime *point = g_date_time_new_from_unix_local(run_at);
    gchar *s = g_date_time_format_iso8601(point);
    log_info("crontabl tool schedule job name = %s, id = %s, run at %s", job_name, job_id, s);
    g_free(s);
    g_date_time_unref(point);
}

static void edc_tool_crontab_schedule(const gchar *workspace, edc_actor_t *scheduler, struct edc_tool *tool, cJSON *jobs)
{
    int n_jobs = cJSON_GetArraySize(jobs);
    if (n_jobs == 0)
    {
        return;
    }

    cJSON *job0 = cJSON_GetArrayItem(jobs, 0);
    cJSON *schedule = cJSON_GetObjectItem(job0, "schedule");
    cJSON *channel = cJSON_GetObjectItem(job0, "channel");

    gboolean once = cJSON_IsTrue(cJSON_GetObjectItem(schedule, "once"));
    gint64 next_time = (gint64)cJSON_GetObjectItem(schedule, "next_run_at_unix_ts")->valuedouble;
    gchar *job_id = cJSON_GetStringValue(cJSON_GetObjectItem(job0, "job_id"));
    gchar *job_name = cJSON_GetStringValue(cJSON_GetObjectItem(job0, "name"));
    gchar *job_message = cJSON_GetStringValue(cJSON_GetObjectItem(job0, "message"));

    for (int i = 1; i < n_jobs; ++i)
    {
        cJSON *job_i = cJSON_GetArrayItem(jobs, i);
        cJSON *schedule_i = cJSON_GetObjectItem(job_i, "schedule");
        gint64 next_time_i = (gint64)(cJSON_GetObjectItem(schedule_i, "next_run_at_unix_ts")->valuedouble);
        gboolean once_i = cJSON_IsTrue(cJSON_GetObjectItem(schedule, "once"));
        gchar *job_name_i = cJSON_GetStringValue(cJSON_GetObjectItem(job_i, "name"));
        cJSON *channel_i = cJSON_GetObjectItem(job_i, "channel");

        if (next_time_i < next_time)
        {
            job_id = cJSON_GetStringValue(job_i);
            job_name = job_name_i;
            job_message = cJSON_GetStringValue(cJSON_GetObjectItem(job_i, "message"));
            next_time = next_time_i;
            once = once_i;
            channel = channel_i;
        }
    }

    struct edc_tool_crontab_state *state = tool->priv;
    if (!state || (next_time < state->timeout_at && !g_str_equal(job_id, state->job_id)))
    {
        edc_tool_crontab_update_task(workspace, edc_channel_from_json(channel), scheduler, tool, job_id, job_name,
                                    job_message, next_time, once);
    }
}

static void edc_tool_crontab_exec_add_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                           void (*callback)(struct edc_toolbox_call_ctx *ctx, GString *, GError *))
{
    cJSON *name = cJSON_GetObjectItem(args, "name");
    cJSON *every_seconds = cJSON_GetObjectItem(args, "every_seconds");
    cJSON *at = cJSON_GetObjectItem(args, "at");
    cJSON *cexpr = cJSON_GetObjectItem(args, "cron_expr");
    cJSON *tz = cJSON_GetObjectItem(args, "tz");
    cJSON *message = cJSON_GetObjectItem(args, "message");

    if (!message)
    {
        gchar *tmp = cJSON_PrintUnformatted(args);
        GError *error = g_error_new(edc_tool_error_quark(), BC_TOOL_ARG_INVALID,
                                    "crontab tool: message is required for action add, args = %s", tmp);
        g_free(tmp);
        callback(ctx, NULL, error);
        return;
    }

    if (!at && !every_seconds && !cexpr)
    {
        gchar *tmp = cJSON_PrintUnformatted(args);
        GError *error =
            g_error_new(edc_tool_error_quark(), BC_TOOL_ARG_INVALID,
                        "'at' / 'every_seconds' / 'cron_expr' should be provided for schedule. args = %s", tmp);
        callback(ctx, NULL, error);
        g_free(tmp);
        return;
    }

    GTimeZone *g_tz = NULL;
    if (tz)
    {
        g_tz = g_time_zone_new_identifier(cJSON_GetStringValue(tz));
        if (!g_tz)
        {
            gchar *tmp = cJSON_PrintUnformatted(tz);
            GError *error = g_error_new(edc_tool_error_quark(), BC_TOOL_ARG_INVALID, "timezone is invalid: %s", tmp);
            g_free(tmp);
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

    GError *error = NULL;
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
        gchar *s = g_strndup(cJSON_GetStringValue(message), 30);
        cJSON_AddStringToObject(job, "name", s);
        g_free(s);
    }

    gchar *job_id = g_uuid_string_random();
    cJSON_AddStringToObject(job, "job_id", job_id);
    g_free(job_id);

    cJSON_AddItemToObject(job, "channel", edc_channel_to_json(ctx->call_from));

    cJSON *schedule = cJSON_AddObjectToObject(job, "schedule");
    cJSON_AddStringToObject(schedule, "tz", g_time_zone_get_identifier(g_tz));

    if (at)
    {
        cJSON_AddStringToObject(schedule, "kind", "at");
        GDateTime *dt = g_date_time_new_from_iso8601(cJSON_GetStringValue(at), g_tz);
        if (!dt)
        {
            gchar *tmp = cJSON_PrintUnformatted(at);
            GError *error =
                g_error_new(edc_tool_error_quark(), BC_TOOL_ARG_INVALID, "'at' is not iso8601 format: %s", tmp);
            callback(ctx, NULL, error);
            g_free(tmp);
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
        gint64 now = g_date_time_to_unix(dt);
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
            GError *e = g_error_new(edc_tool_error_quark(), BC_TOOL_ARG_INVALID, "parse cron expr %s failed: %s",
                                    cJSON_GetStringValue(cexpr), error);
            callback(ctx, NULL, e);

            g_time_zone_unref(g_tz);
            return;
        }

        gint interval = g_time_zone_find_interval(g_tz, G_TIME_TYPE_STANDARD, g_get_real_time() / 1000000);
        gint32 tz_offset = g_time_zone_get_offset(g_tz, interval);

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
        GString *result = g_string_new(NULL);
        gchar *s = cJSON_PrintUnformatted(job);
        g_string_append_printf(result, "Add job %s done. (exit code: 0)", s);
        g_free(s);
        callback(ctx, result, NULL);
    }

    g_time_zone_unref(g_tz);
    cJSON_Delete(db);
}

static void edc_tool_crontab_exec_async(struct edc_toolbox_call_ctx *ctx, struct edc_tool *tool, cJSON *args,
                                       void (*callback)(struct edc_toolbox_call_ctx *, GString *, GError *))
{
    gchar *action = cJSON_GetStringValue(cJSON_GetObjectItem(args, "action"));
    if (g_str_equal(action, "add"))
    {
        edc_tool_crontab_exec_add_async(ctx, tool, args, callback);
    }
    else if (g_str_equal(action, "list"))
    {
        edc_tool_crontab_exec_list_async(ctx, tool, args, callback);
    }
    else if (g_str_equal(action, "remove"))
    {
        edc_tool_crontab_exec_remove_async(ctx, tool, args, callback);
    }
}

static void edc_tool_create_crontab(struct edc_toolbox *tb)
{
    GSList *params = NULL;
    params = g_slist_append(params, edc_tool_param_new("action",
                                                       edc_param_str_type("Action to perform", "add", "list", "remove", NULL), TRUE));
    params = g_slist_append(params,
                            edc_tool_param_new("message", edc_param_str_type("Reminder message (for add)", NULL), FALSE));

    params = g_slist_append(params, edc_tool_param_new("every_seconds",
                                                       edc_param_int_type("Interval in seconds (for recurring task)"), FALSE));
    params =
        g_slist_append(params, edc_tool_param_new("cron_expr",
                                                  edc_param_str_type("Cron expression like '0 2 * * *' (for scheduled tasks)", NULL), FALSE));

    params = g_slist_append(params, edc_tool_param_new("tz",
                                                       edc_param_str_type("IANA timezone for cron expressions (e.g. 'China/Beijing')", NULL), FALSE));

    params = g_slist_append(params,
                            edc_tool_param_new("at",
                                              edc_param_str_type("ISO datetime for one-time execution (e.g. '2026-05-01T12:00:00')", NULL), FALSE));

    params = g_slist_append(params, edc_tool_param_new("job_id", edc_param_str_type("Job ID (for remove)", NULL), FALSE));
    params = g_slist_append(params, edc_tool_param_new("name", edc_param_str_type("Job Name (for add)", NULL), FALSE));

    struct edc_tool *tool = edc_tool_new("crontab", "Schedule reminders and recurring tasks. Actions: add, list, remove.",
                                       params, edc_tool_crontab_exec_async);

    tool->priv = NULL;
    tool->priv_free_fn = (GFreeFunc)edc_tool_crontab_state_free;

    edc_toolbox_register(tb, tool);

    GError *error = NULL;
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
