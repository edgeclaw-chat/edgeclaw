#include "edc_history.h"
#include "log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

gchar *edc_get_timestamp(void)
{
    GDateTime *now = g_date_time_new_now_local();
    gchar *time = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref(now);
    return time;
}

cJSON *edc_history_load(const gchar *workspace, const size_t offset)
{
    gchar *history_file = g_build_filename(workspace, "history.jsonl", NULL);
    FILE *fin = fopen(history_file, "r");
    g_free(history_file);

    if (fin == NULL)
    {
        return NULL;
    }

    cJSON *history_array = cJSON_CreateArray();
    gchar buf[1024];
    GString *line = g_string_new(NULL);
    size_t line_counts = 0;

    while (fgets(buf, sizeof(buf), fin) != NULL)
    {
        g_string_append(line, buf);
        if (line->len > 0 && line->str[line->len - 1] == '\n')
        {
            line_counts += 1;
            if (line_counts < offset)
            {
                g_string_truncate(line, 0);
                continue;
            }

            g_strstrip(line->str);
            line->len = strlen(line->str);
            if (line->len > 0)
            {
                cJSON *msg = cJSON_Parse(line->str);
                if (msg != NULL)
                {
                    cJSON_AddItemToArray(history_array, msg);
                }
            }
            g_string_truncate(line, 0);
        }
    }

    if (line->len > 0 && line_counts >= offset)
    {
        cJSON *msg = cJSON_Parse(line->str);
        if (msg != NULL)
        {
            cJSON_AddItemToArray(history_array, msg);
        }
    }

    g_string_free(line, TRUE);
    fclose(fin);
    return history_array;
}

gboolean edc_history_save(const gchar *workspace, cJSON *msg_lst, GList *timestamps, gsize skip_count)
{
    gchar *hist_file = g_build_filename(workspace, "history.jsonl", NULL);
    FILE *fout = fopen(hist_file, "a+");
    if (fout == NULL)
    {
        char *s = strerror(errno);
        log_info("open file %s failed: %s", hist_file, s);
        g_free(hist_file);
        return FALSE;
    }

    for (int i = skip_count, t = 0; i < cJSON_GetArraySize(msg_lst); i++, t++)
    {
        cJSON *msg = cJSON_GetArrayItem(msg_lst, i);
        const gchar *timestamp = NULL;
        if (timestamps != NULL)
        {
            timestamp = (const gchar *)g_list_nth_data(timestamps, t);
            if (timestamp)
            {
                cJSON_AddStringToObject(msg, "timestamp", timestamp);
            }
        }
        char *line = cJSON_PrintUnformatted(msg);

        if (timestamp)
        {
            cJSON_DeleteItemFromObject(msg, "timestamp");
        }

        fprintf(fout, "%s\n", line);
        free(line);
    }

    fflush(fout);
    fclose(fout);
    g_free(hist_file);
    return TRUE;
}

gboolean edc_history_get_info(const gchar *workspace, size_t *compress_index, gchar **update_time)
{
    gchar *history_info_path = g_build_filename(workspace, "history_info.json", NULL);
    GError *error = NULL;
    gchar *info = NULL;
    cJSON *history_info = NULL;

    if (!g_file_test(history_info_path, G_FILE_TEST_EXISTS))
    {
        *compress_index = 0;
        if (update_time)
            *update_time = edc_get_timestamp();
        g_free(history_info_path);
        return TRUE;
    }

    gboolean ret = TRUE;
    gboolean ok = g_file_get_contents(history_info_path, &info, NULL, &error);

    if (!ok)
    {
        log_error("load history info failed: %s", error->message);
        ret = FALSE;
        goto exit_;
    }

    history_info = cJSON_Parse(info);
    if (!history_info)
    {
        log_error("load history info failed. format error: %s", info);
        ret = FALSE;
        goto exit_;
    }

    cJSON *compress_index_obj = cJSON_GetObjectItem(history_info, "compress_index");
    cJSON *update_time_obj = cJSON_GetObjectItem(history_info, "update_time");
    if (!compress_index_obj || !update_time_obj || !cJSON_IsNumber(compress_index_obj) ||
        !cJSON_IsString(update_time_obj))
    {
        log_error("load history info failed. format error: %s", info);
        ret = FALSE;
        goto exit_;
    }

    *compress_index = (size_t)cJSON_GetNumberValue(compress_index_obj);
    if (update_time)
    {
        *update_time = g_strdup(cJSON_GetStringValue(update_time_obj));
    }

exit_:
    if (error)
    {
        g_error_free(error);
    }
    if (info)
    {
        g_free(info);
    }
    g_free(history_info_path);
    cJSON_Delete(history_info);

    return ret;
}

gboolean edc_history_set_info(const gchar *workspace, size_t compress_index, const gchar *update_time)
{
    gchar *history_info_path = g_build_filename(workspace, "history_info.json", NULL);
    FILE *fout = fopen(history_info_path, "w+");
    g_free(history_info_path);

    if (fout == NULL)
    {
        log_error("open history info file failed: %s", strerror(errno));
        return FALSE;
    }

    cJSON *history_info = cJSON_CreateObject();
    cJSON_AddStringToObject(history_info, "update_time", update_time);
    cJSON_AddNumberToObject(history_info, "compress_index", compress_index);
    gchar *info = cJSON_PrintUnformatted(history_info);
    cJSON_Delete(history_info);

    fprintf(fout, "%s", info);
    fflush(fout);
    fclose(fout);
    cJSON_free(info);
    return TRUE;
}
