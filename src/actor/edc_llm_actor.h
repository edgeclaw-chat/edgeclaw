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
    /* The in-flight turn's messages that still have to be sent, as an OWNED array: from
     * its folded_prefix to the end. It used to be (EdcQueue *turn, turn_begin) pointing
     * into the live turn; messages are rows now, so the producer loads the span and hands
     * the array over. NULL = nothing to append. */
    cJSON *turn_tail;
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
extern struct edc_llm_actor_req *edc_llm_actor_req_new(cJSON *msg_lst, cJSON *tools);
extern void edc_llm_actor_req_free(struct edc_llm_actor_req *req);
extern struct edc_llm_actor_resp *edc_llm_actor_resp_new();
extern void edc_llm_actor_resp_free(struct edc_llm_actor_resp *resp);

/* Whether a chat-completions message key may travel to the provider. Everything else in a
 * stored message is ours (the folded-paste payload, a tool-set delta's bookkeeping) and is
 * dropped on the way out -- see llm_add_wire_message. Exposed so a test can pin the policy. */
extern bool edc_llm_wire_key_is_standard(const char *key);

#endif
