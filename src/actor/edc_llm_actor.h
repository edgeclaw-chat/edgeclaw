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
    /* 本 turn 的发送起点 (0 = 整条): 折叠若切在 turn 内部, 其前缀已在组记忆里,
     * 不能再发一遍。生产者 (edc_llm_request) 按 active_turn->folded_prefix 置位。 */
    size_t turn_begin;
    cJSON *history;
    cJSON *tools;
    bool history_owned; /* Phase 5: true = history needs cJSON_Delete */
    cJSON *tail_parts;  /* owned by req: role=system 消息的**数组**, send_req 时按序
                           追加为 messages 绝对队尾 (在 turn 之后)。装在里面的都是
                           "请求期现算、只对这一次生效"的运行时提醒:
                           [0] L2 session 记忆通知 (若有), [1] todo reminder (节流后)。
                           NULL 或空数组 = 不追加。 */
    int max_tokens;     /* > 0 = 请求体带 "max_tokens" (只有折叠的摘要调用用得上:
                           摘要必须能在 reserve 里装下, 见 EDC_FOLD_SUMMARY_MAX_TOKENS)。
                           普通对话回复不设上限。0 = 不发这个字段 */
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
