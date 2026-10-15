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
    /* How many leading messages of this turn have already entered the session memory
     * (L2), by ANY path (fold-chained curation or the idle-group extraction). It is a
     * pure extraction watermark: it NEVER participates in request assembly, the group
     * history or the fold corpus. Invariant: folded_prefix <= memo_prefix <= turn_len,
     * and both writers (extraction landing, fold landing clamp) only ever raise it. */
    int memo_prefix;
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
/* An assistant message asking for ONE tool call, in exactly the shape the provider
 * returns -- so a seeded call (a `!cmd` task, see edc_task_spec.initial_tool_name) is
 * indistinguishable from one the model asked for. Required next to every tool message:
 * the request/fold paths rely on each `role=tool` being preceded by its
 * `assistant(tool_calls)` (see edc_llm_request.c). |args| is the JSON argument object
 * and is serialized into the API's `arguments` STRING field; NULL = "{}". */
extern void edc_turn_add_assistant_tool_call(struct edc_turn *turn, const char *call_id, const char *name,
                                             cJSON *args);
#endif
