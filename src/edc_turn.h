#ifndef __BC_TURN_H__
#define __BC_TURN_H__
#include "edc_containers.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_turn
{
    char *task_id;             /* Phase 6: 序列化时标记归属 */
    EdcQueue messages;
    EdcQueue timestamps;

    /* Phase 3: message range in session->messages (recorded at flush time) */
    size_t msg_range_start;    /* index of first message in session->messages */
    size_t msg_range_count;    /* number of messages */
    bool   range_valid;        /* true if range has been recorded */
};

extern struct edc_turn *edc_turn_new();
extern void edc_turn_free(struct edc_turn *turn);

extern void edc_turn_add_user_message(struct edc_turn *turn, const char *content);
extern void edc_turn_add_assistant_message(struct edc_turn *turn, cJSON *msg);
extern void edc_turn_add_tool_message(struct edc_turn *turn, const char *tool_call_id, const char *name,
                                     const char *content);
#endif
