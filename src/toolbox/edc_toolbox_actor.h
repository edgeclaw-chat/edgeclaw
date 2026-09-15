#include "sds.h"
#ifndef __BC_LLM_TOOLBOX_ACTOR_H__
#define __BC_LLM_TOOLBOX_ACTOR_H__

#include "edc_actor.h"
#include "edc_toolbox.h"

typedef struct
{
    struct edc_toolbox *toolbox;
    /* 仍在执行中的工具调用 (异步工具回调还没触发)。teardown 时 actor loop
     * 会被直接停掉, 这些回调用永远不会再跑, 若不在此登记释放, 调用上下文
     * (及其持有的 req 链 / actor msg) 就会随进程退出泄漏。 */
    struct edc_toolbox_call_ctx **inflight;
    size_t inflight_count;
    size_t inflight_cap;
} edc_toolbox_actor_state_t;

struct edc_task; /* fwd */

/* struct edc_toolbox_actor_req 已移至 edc_exec_io.h (executor 的 I/O 词表)。 */

extern edc_actor_t *edc_toolbox_actor_new(struct edc_toolbox *toolbox);
extern void edc_toolbox_actor_free(edc_actor_t *actor);
extern struct edc_toolbox_actor_req *edc_toolbox_actor_req_new(sds name, cJSON *args, sds call_id,
                                                               edc_channel_type call_from_type, struct edc_task *task);
extern void edc_toolbox_actor_req_free(struct edc_toolbox_actor_req *req);
#endif
