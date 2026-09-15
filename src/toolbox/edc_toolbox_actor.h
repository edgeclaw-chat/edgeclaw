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

/* ── 请求侧: "LLM 回了什么" → toolbox 请求链 ─────────────────
 *
 * 响应侧是 actor 自己的 handler (`BC_ACTOR_MSG_TOOL_REQUEST` → 执行 → 结果回发);
 * 这里是同一份协议的**请求方向** —— req 链的形状、`BC_ACTOR_MSG_TOOL_REQUEST`
 * 的装配、以及"按 task 账本重建尚未执行的请求"(resume) 都归它, 所以与 actor 同住。
 *
 * 调用方 (scheduler / executor) 只声明"该派工具了 / 该重派了"。
 * 思维内容回显仍走 `sched->send_reply` 端口 —— 内容上屏是**通道**的活。
 * `redispatch` 返回重建并投递的请求条数 (0 = 无事可做)。 */
struct edc_scheduler; /* fwd */
extern void edc_toolbox_actor_dispatch(edc_actor_t *actor, struct edc_scheduler *sched, struct edc_task *task,
                                       void *llm_resp, void *request_id);
extern int edc_toolbox_actor_redispatch(edc_actor_t *actor, struct edc_scheduler *sched, struct edc_task *task,
                                        void *request_id);

/* ── 端口: toolbox 是两条工具端口的提供方 ────────────────────
 *
 * 工具协议 (req 链 / dispatch / 按账本重建) 全住本文件, 所以"把端口装到
 * scheduler 上"也归这里 —— **谁提供谁接线**。装配根只在装配期叫这一次,
 * 运行期不再出现在路径上 (`dispatch_tools` / `redispatch_tools`)。
 * 端口签名一个字没动: kernel 的调用点与测试里的假端口都不受影响。 */
extern void edc_toolbox_actor_bind_scheduler(edc_actor_t *actor, struct edc_scheduler *sched);
#endif
