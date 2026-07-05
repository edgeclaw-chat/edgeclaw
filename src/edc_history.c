#include "sds.h"
#include "edc_history.h"
#include "edc_session.h"
#include "edc_datetime_utils.h"
#include "edc_file_utils.h"
#include "log.h"
#include "edc_compat.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

sds edc_get_timestamp(void)
{
    return edc_datetime_timestamp();
}

cJSON *edc_history_load(const char *workspace, const size_t offset)
{
    sds history_file = edc_path_join(workspace, "history.jsonl", NULL);
    FILE *fin = fopen(history_file, "r");
    sdsfree(history_file);

    if (fin == NULL)
    {
        return NULL;
    }

    cJSON *history_array = cJSON_CreateArray();
    char *raw_line = NULL;
    size_t raw_cap = 0;
    ssize_t raw_len;
    size_t line_counts = 0;

    while ((raw_len = getline(&raw_line, &raw_cap, fin)) != -1)
    {
        line_counts += 1;
        if (line_counts < offset)
            continue;

        sds line = sdsnewlen(raw_line, (size_t)raw_len);
        line = sdstrim(line, " \t\r\n");
        sdssetlen(line, sdslen(line));
        if (sdslen(line) > 0)
        {
            cJSON *msg = cJSON_Parse(line);
            if (msg != NULL)
            {
                cJSON_AddItemToArray(history_array, msg);
            }
        }
        sdsfree(line);
    }

    free(raw_line);
    fclose(fin);
    return history_array;
}

bool edc_history_save(const char *workspace, cJSON *msg_lst, struct edc_timestamp_node *timestamps, size_t skip_count)
{
    sds hist_file = edc_path_join(workspace, "history.jsonl", NULL);
    /* auto-create directory */
    sds dir = edc_path_join(workspace, NULL);
    mkdir(dir, 0755);
    sdsfree(dir);

    FILE *fout = fopen(hist_file, "a+");
    if (fout == NULL)
    {
        char *s = strerror(errno);
        log_info("open file %s failed: %s", hist_file, s);
        sdsfree(hist_file);
        return false;
    }

    for (int i = skip_count, t = 0; i < cJSON_GetArraySize(msg_lst); i++, t++)
    {
        cJSON *msg = cJSON_GetArrayItem(msg_lst, i);
        const char *timestamp = NULL;
        if (timestamps != NULL)
        {
            struct edc_timestamp_node *iter = timestamps;
            for (int j = 0; j < t && iter != NULL; j++)
                iter = iter->next;
            if (iter)
            {
                timestamp = iter->ts;
                cJSON_AddStringToObject(msg, "timestamp", timestamp);
            }
        }
        char *line = cJSON_PrintUnformatted(msg);

        if (timestamp)
        {
            cJSON_DeleteItemFromObject(msg, "timestamp");
        }

        fprintf(fout, "%s\n", line);
        cJSON_free(line);
    }

    fflush(fout);
    fclose(fout);
    sdsfree(hist_file);
    return true;
}

bool edc_history_get_info(const char *workspace, size_t *compress_index, sds *update_time)
{
    sds history_info_path = edc_path_join(workspace, "history_info.json", NULL);

    cJSON *history_info = NULL;

    if (!edc_file_exists(history_info_path))
    {
        *compress_index = 0;
        if (update_time)
            *update_time = edc_get_timestamp();
        sdsfree(history_info_path);
        return true;
    }

    bool ret = true;
    EdcError *error = NULL;
    sds info = edc_file_read(history_info_path, &error);

    if (!info)
    {
        log_error("load history info failed: %s", error->message);
        ret = false;
        goto exit_;
    }

    history_info = cJSON_Parse(info);
    if (!history_info)
    {
        log_error("load history info failed. format error: %s", info);
        ret = false;
        goto exit_;
    }

    cJSON *compress_index_obj = cJSON_GetObjectItem(history_info, "compress_index");
    cJSON *update_time_obj = cJSON_GetObjectItem(history_info, "update_time");
    if (!compress_index_obj || !update_time_obj || !cJSON_IsNumber(compress_index_obj) ||
        !cJSON_IsString(update_time_obj))
    {
        log_error("load history info failed. format error: %s", info);
        ret = false;
        goto exit_;
    }

    *compress_index = (size_t)cJSON_GetNumberValue(compress_index_obj);
    if (update_time)
    {
        *update_time = sdsnew(cJSON_GetStringValue(update_time_obj));
    }

exit_:
    if (error)
    {
        edc_error_free(error);
    }
    if (info)
    {
        sdsfree(info);
    }
    sdsfree(history_info_path);
    cJSON_Delete(history_info);

    return ret;
}

bool edc_history_set_info(const char *workspace, size_t compress_index, const char *update_time)
{
    sds history_info_path = edc_path_join(workspace, "history_info.json", NULL);
    FILE *fout = fopen(history_info_path, "w+");
    sdsfree(history_info_path);

    if (fout == NULL)
    {
        log_error("open history info file failed: %s", strerror(errno));
        return false;
    }

    cJSON *history_info = cJSON_CreateObject();
    cJSON_AddStringToObject(history_info, "update_time", update_time);
    cJSON_AddNumberToObject(history_info, "compress_index", compress_index);
    char *info = cJSON_PrintUnformatted(history_info);
    cJSON_Delete(history_info);

    fprintf(fout, "%s", info);
    fflush(fout);
    fclose(fout);
    cJSON_free(info);
    return true;
}
