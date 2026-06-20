#ifndef __BC_HISTORY_H__
#define __BC_HISTORY_H__

#include <cJSON.h>
#include <glib.h>

extern cJSON *edc_history_load(const gchar *workspace, size_t offset);
extern gboolean edc_history_save(const gchar *workspace, cJSON *msg_lst, GList *timestamps, gsize skip_count);
extern gboolean edc_history_get_info(const gchar *workspace, size_t *compress_index, gchar **update_time);
extern gboolean edc_history_set_info(const gchar *workspace, size_t compress_index, const gchar *update_time);
extern gchar *edc_get_timestamp(void);

#endif
