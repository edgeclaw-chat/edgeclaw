#ifndef __BC_HISTORY_H__
#define __BC_HISTORY_H__

#include <cJSON.h>
#include <glib.h>
#include <stdbool.h>

extern cJSON *edc_history_load(const char *workspace, size_t offset);
extern bool edc_history_save(const char *workspace, cJSON *msg_lst, GList *timestamps, size_t skip_count);
extern bool edc_history_get_info(const char *workspace, size_t *compress_index, char **update_time);
extern bool edc_history_set_info(const char *workspace, size_t compress_index, const char *update_time);
extern char *edc_get_timestamp(void);

#endif
