#ifndef __BC_CONTEXT_H__
#define __BC_CONTEXT_H__

#include "edc_config.h"
#include "sds.h"
#include <cJSON.h>

struct edc_task;
struct edc_task_group;

/* 基础 system prompt: identity 模板 + bootstrap 文件 + MEMORY + skills。
 * 返回新 sds, 调用方负责 sdsfree。 */
extern sds edc_context_build_system_prompt(struct edc_config *conf);

/* task 通过 update_todolist 工具持有非空 todo 清单 (task->todolist) 时, 把
 * "引导 + 当前执行状态摘要 + 清单行"区块追加到 base 系统提示文本尾部, 生成
 * 新的 system 消息对象 (role=system)。副本语义——不改组内共享的 history[0]。
 * 无 todo / 空清单 / task 为 NULL 时返回 NULL (调用方自行决定用 base 原样)。
 * 调用方负责 cJSON_Delete。(自 edc_agent.c / edc_agent_todo_inject.h 迁入) */
extern cJSON *edc_context_make_sys_with_todo(const char *base_content, struct edc_task *task);

/* 把本次 LLM 请求的 "system 部分" 组装并追加到 msgs 数组尾部, 返回追加条数:
 *   [0] (可选) group memory 摘要消息 (## Group Memory), group->memory 非空才有
 *   [1] system prompt:
 *       - group 有 history: base = history[0].content; task 带 todo 时追加注入
 *         副本 (owned, 随 msgs 释放), 否则以引用挂 history[0] (不复制, msgs
 *         释放不影响 group)
 *       - group 无 history / group 为 NULL: 需 conf 非空才以
 *         edc_context_build_system_prompt 现拼; task 带 todo 时请求里发注入
 *         副本, 否则发纯净文本; 纯净文本同时经 *out_pristine (可 NULL) 带回,
 *         由调用方决定是否写回 group->history (保持组内共享 prompt 纯净)
 *   [2] (可选) todo 规划引导 (## Task Planning, 短 system 消息): 仅当
 *       task 非 NULL 且尚无 todo 清单时追加——提醒 LLM 面对多步任务先用
 *       update_todolist 建清单规划; 已有清单时该引导不出现 (换由 [1] 副本里
 *       的 kTodoSysGuide 维护规则接管)。task 为 NULL (非任务请求) 不追加。
 * 引用/副本/写回语义与迁移前的 edc_agent_cb_send_llm 完全一致。 */
extern int edc_context_append_system_part(cJSON *msgs, struct edc_config *conf,
                                          struct edc_task_group *group, struct edc_task *task,
                                          sds *out_pristine);

#endif
