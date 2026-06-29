#ifndef __BC_TURN_H__
#define __BC_TURN_H__
#include <cJSON.h>
#include <glib.h>

struct edc_turn
{
    GQueue *messages;
    GQueue *timestamps;
};

extern struct edc_turn *edc_turn_new();
extern void edc_turn_free(struct edc_turn *turn);

extern void edc_turn_add_user_message(struct edc_turn *turn, const char *content);
extern void edc_turn_add_assistant_message(struct edc_turn *turn, cJSON *msg);
extern void edc_turn_add_tool_message(struct edc_turn *turn, const char *tool_call_id, const char *name,
                                     const char *content);
#endif
