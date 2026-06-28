#include "edc_string.h"
#include "utarray.h"
#ifndef __BC_LLM_ACTOR_H__
#define __BC_LLM_ACTOR_H__
#include "edc_actor.h"
#include "edc_config.h"
#include "edc_containers.h"
#include "edc_error.h"
#include "edc_network_async.h"
#include <cJSON.h>
typedef struct
{
    struct llm_api_conf api_conf;
    edc_network_async_t *network;
} edc_llm_actor_state_t;

struct edc_llm_actor_req
{
    EdcQueue *turn;
    cJSON *history;
    cJSON *tools;
};

typedef enum
{
    BC_LLM_FINISH_REASON_STOP,
    BC_LLM_FINISH_REASON_TOOL_CALL,
    BC_LLM_FINISH_REASON_LENGTH,
    BC_LLM_FINISH_REASON_SENSITIVE,
    BC_LLM_FINISH_REASON_NETWORK_ERROR,
    BC_LLM_FINISH_REASON_CONTEXT_EXCEEDED,
    BC_LLM_FINISH_REASON_UNKNOWN,
} edc_llm_actor_finish_reason_t;

typedef enum
{
    BC_LLM_CHAT_ROLE_USER,
    BC_LLM_CHAT_ROLE_ASSISTANT,
    BC_LLM_CHAT_ROLE_SYSTEM,
} edc_llm_actor_role_t;

typedef enum
{
    BC_LLM_TOOL_CALL_TYPE_FUNCTION,
} edc_llm_actor_tool_call_type_t;

struct edc_llm_chat_tool_call
{
    sds id;
    edc_llm_actor_tool_call_type_t type;
    size_t index;
    struct
    {
        sds name;
        cJSON *arguments;
    } function;
};

struct edc_llm_actor_resp
{
    edc_llm_actor_finish_reason_t reason;
    size_t index;
    sds content;
    sds reasoning_content;
    edc_llm_actor_role_t role;

    UT_array *tool_calls;
    cJSON *msg;
    EdcError *error;
};

extern edc_actor_t *edc_llm_actor_new(struct llm_api_conf *api_conf, edc_network_async_t *network);
extern void edc_llm_actor_free(edc_actor_t *actor);
extern struct edc_llm_actor_req *edc_llm_actor_req_new(cJSON *msg_lst, cJSON *tools, EdcQueue *turn);
extern void edc_llm_actor_req_free(struct edc_llm_actor_req *req);
extern struct edc_llm_actor_resp *edc_llm_actor_resp_new();
extern void edc_llm_actor_resp_free(struct edc_llm_actor_resp *resp);

#endif
