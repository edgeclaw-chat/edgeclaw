#include "sds.h"
#ifndef __BC_LLM_TOOLBOX_ACTOR_H__
#define __BC_LLM_TOOLBOX_ACTOR_H__

#include "edc_actor.h"
#include "edc_toolbox.h"

typedef struct
{
    struct edc_toolbox *toolbox;
} edc_toolbox_actor_state_t;

struct edc_task; /* fwd */

struct edc_toolbox_actor_req
{
    sds name;
    cJSON *args;
    sds call_id;
    edc_channel_type call_from_type;
    struct edc_task *task; /* 发起本次工具调用的 task (agent 注入；NULL = 非 task 路径) */
    sds result;
    struct edc_toolbox_actor_req *next;
};

extern edc_actor_t *edc_toolbox_actor_new(struct edc_toolbox *toolbox);
extern void edc_toolbox_actor_free(edc_actor_t *actor);
extern struct edc_toolbox_actor_req *edc_toolbox_actor_req_new(sds name, cJSON *args, sds call_id,
                                                               edc_channel_type call_from_type,
                                                               struct edc_task *task);
extern void edc_toolbox_actor_req_free(struct edc_toolbox_actor_req *req);
#endif
