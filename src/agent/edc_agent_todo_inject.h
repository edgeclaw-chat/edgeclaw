#ifndef __BC_AGENT_TODO_INJECT_H__
#define __BC_AGENT_TODO_INJECT_H__

#include "sds.h"
#include <cJSON.h>

struct edc_task;

/* task 通过 update_todolist 工具持有非空 todo 清单 (task->todolist) 时，
 * 把"引导 + 当前执行状态摘要 + 清单行"区块追加到该 task 的 system prompt
 * 副本尾部 (edc_agent_cb_send_llm 每次组装请求时调用)：让模型每轮都看到
 * 最新进度，并提醒它及时调用 update_todolist 同步。无 todo 时返回 NULL。
 * 返回的是未挂任何数组的新 system 消息对象 (副本语义——不改组内共享的
 * history[0])。调用方负责 cJSON_Delete。实现于 edc_agent.c。 */
extern cJSON *edc_agent_make_sys_with_todo(const char *base_content, struct edc_task *task);

#endif
