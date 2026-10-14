#ifndef __BC_TURN_H__
#define __BC_TURN_H__
#include "edc_containers.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_turn
{
    char *task_id; /* marks ownership when serialized */
    EdcQueue messages;
    /* How many leading messages of this turn the group memory already covers (L1
     * fold). Request assembly and the group history both start after them; set at
     * fold landing, persisted with the turn. 0 = nothing folded. */
    int folded_prefix;
};

extern struct edc_turn *edc_turn_new();
extern void edc_turn_free(struct edc_turn *turn);

/* Append one full message (takes ownership); cleans UTF-8 and fills ts/render. */
extern void edc_turn_add_message(struct edc_turn *turn, cJSON *msg);
extern void edc_turn_add_user_message(struct edc_turn *turn, const char *content);
extern void edc_turn_add_assistant_message(struct edc_turn *turn, cJSON *msg);
/* A tool message = the tool result in the LLM's context (tool_call_id/name/content
 * is all the request needs). */
extern void edc_turn_add_tool_message(struct edc_turn *turn, const char *tool_call_id, const char *name,
                                      const char *content);
#endif
