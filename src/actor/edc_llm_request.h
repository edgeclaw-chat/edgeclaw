#ifndef __EDC_LLM_REQUEST_H__
#define __EDC_LLM_REQUEST_H__

#include "edc_actor.h" /* edc_actor_t */
#include <cJSON.h>

struct edc_scheduler;
struct edc_task;

/* 装配一次 LLM 请求并投给 llm_actor 邮箱（原 src/assembly/edc_runtime.c 的搬家）。
 *
 * kernel 自己的东西（group 历史 / 折叠水位 / deps / active_turn / todo 节流 /
 * config / session 仓库）全部经 sched 与 task 取得；另有两样 kernel 拿不到的
 * 值由装配根在装配期**当参数**交进来（不新增 core 字段）：
 *   tool_schemas —— 工具定义数组; 所有权归 toolbox (edc_toolbox_free 释放),
 *                   本函数只读不释放; 非 NULL 且 sink!=GROUP_MEMORY 时进请求体;
 *                   传 NULL = 这次请求不带工具 (折叠摘要调用)
 *   llm_actor    —— LLM 端点邮箱 (所有权归装配根)
 *
 * $VAR 的"值语义"($$ / $LAST / $N / $NAME) 是本文件私有的 static resolver,
 * 数据源 = sched->sessions (store 句柄) —— 见 docs/runtime-model.md §11。 */
extern void edc_llm_request_send(struct edc_scheduler *sched, struct edc_task *task, cJSON *tool_schemas,
                                 edc_actor_t *llm_actor, void *request_id);

#endif
