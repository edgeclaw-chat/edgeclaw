#include "sds.h"
#include "utarray.h"
#ifndef __BC_LLM_ACTOR_H__
#define __BC_LLM_ACTOR_H__
#include "edc_actor.h"
#include "edc_config.h"
#include "edc_containers.h"
#include "edc_error.h"
#include "edc_exec_io.h"
#include "edc_https_client.h"
#include <cJSON.h>
typedef struct
{
    struct llm_api_conf api_conf;
    struct edc_https_client *client;
} edc_llm_actor_state_t;

struct edc_llm_actor_req
{
    EdcQueue *turn;
    /* This turn's send start (0 = the whole turn): if a fold cut inside the turn, its
     * prefix is already in the group memory and must not be sent again. The producer
     * (edc_llm_request) sets it from active_turn->folded_prefix. */
    size_t turn_begin;
    cJSON *history;
    cJSON *tools;
    /* Stable per-conversation key for the provider's automatic prefix cache (OpenAI's
     * `prompt_cache_key`): one conversation's requests then share a cache shard instead of
     * landing on whichever shard the load balancer picks. The producer sets it from the
     * group id; NULL = the field is not sent. Freed with the request. */
    sds cache_key;
    bool history_owned; /* Phase 5: true = history needs cJSON_Delete */
    cJSON *tail_parts;  /* owned by req: an ARRAY of role=system messages appended in
                           order at the absolute tail of messages (after the turn).
                           Everything in it is a request-time runtime reminder valid
                           only for this call: [0] the L2 session-memory notice (if any),
                           [1] the todo reminder (after throttling). NULL/empty = none. */
    int max_tokens;     /* > 0 = the body carries "max_tokens" (only the fold's summary
                           call needs it: the summary must fit in the reserve, see
                           EDC_FOLD_SUMMARY_MAX_TOKENS). A normal reply sets no cap;
                           0 = do not send the field */
};


/* Payload for BC_ACTOR_MSG_LLM_SET_CONFIG: new provider settings. Any field
 * may be NULL/empty to leave it unchanged. Owned by the message. */
struct edc_llm_api_conf_update
{
    sds api_url;
    sds model;
    sds api_key;
};
void edc_llm_api_conf_update_free(void *ptr);

extern edc_actor_t *edc_llm_actor_new(struct edc_config *config);
extern void edc_llm_actor_free(edc_actor_t *actor);
extern struct edc_llm_actor_req *edc_llm_actor_req_new(cJSON *msg_lst, cJSON *tools, EdcQueue *turn);
extern void edc_llm_actor_req_free(struct edc_llm_actor_req *req);
extern struct edc_llm_actor_resp *edc_llm_actor_resp_new();
extern void edc_llm_actor_resp_free(struct edc_llm_actor_resp *resp);

#endif
