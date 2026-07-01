#ifndef __BC_HISTORY_H__
#define __BC_HISTORY_H__

#include "sds.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_timestamp_node;

extern cJSON *edc_history_load(const char *workspace, size_t offset);
extern bool edc_history_save(const char *workspace, cJSON *msg_lst, struct edc_timestamp_node *timestamps, size_t skip_count);
extern bool edc_history_get_info(const char *workspace, size_t *compress_index, sds *update_time);
extern bool edc_history_set_info(const char *workspace, size_t compress_index, const char *update_time);
extern sds edc_get_timestamp(void);

#endif
