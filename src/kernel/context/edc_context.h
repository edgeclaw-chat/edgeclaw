#ifndef __BC_CONTEXT_H__
#define __BC_CONTEXT_H__

#include "edc_config.h"
#include "sds.h"
#include <cJSON.h>

struct edc_task;
struct edc_task_group;

/* 基础 system prompt: identity 模板 + bootstrap 文件 + MEMORY + skills。
 * |workdir| 是 session 绑定的工作目录 (绝对路径), 作为 identity 里
 * "## Current Running Directory" 的值; NULL/空则回退到进程 cwd。
 * 返回新 sds, 调用方负责 sdsfree。 */
extern sds edc_context_build_system_prompt(struct edc_config *conf, const char *workdir);

/* 运行时 todo reminder (role=system 的新消息, 绝对新鲜)。调用方
 * (edc_runtime_cb_send_llm) 按节流规则决定是否调用本函数: 只在"隔了
 * EDC_TODO_SYNC_REMIND_GAP 条对话 (LLM 请求) 都没调用过 todowrite"时
 * 调用 (edc_task_llm_request_poke 达阈值), 并把返回的消息塞到该请求消息列表
 * 的【最末尾】(新的队尾, 见 edc_llm_actor_req.tail_system), 避免头部注入的
 * todo 状态在长上下文里被模型遗忘。content 即两段式骨架文本
 * (含 <system-reminder> 包裹, 由 edc_context_render_todo_reminder 统一生成,
 * 与 todowrite 工具返回的 result 逐字节一致——同一渲染源):
 *   ① ## 🚨 CURRENT TASK SYSTEM STATE: 状态段——当前清单全貌 (清单行);
 *   ② ## 🛠️ INSTRUCTIONS FOR THIS TURN: 指令段——逐轮强约束的同步节拍
 *      (检查上个工具结果; 当前条目干完 → 下一动作必须是 todowrite;
 *      未标 [x] 不得跳下一项)。解决"执行完好几步才更新一次清单"的滞后。
 * 两个分支 (同一骨架、互斥出现, 由 render_reminder 按 task->todos 选择):
 *   - task 持有非空 todo 清单 → 状态段 = 清单行; 指令段 = 逐轮同步节拍;
 *   - task 非 NULL 但尚无/已清空清单 → 状态段 = "尚无清单"; 指令段 =
 *     多步任务先调用 todowrite 建计划;
 *   - task 为 NULL (非任务请求) → NULL (不打扰)。
 * 副本语义: 返回未挂到任何数组的新 cJSON 对象, 随请求释放; 绝不改动
 * task/group 共享数据 (task->todos 只读)。调用方负责加入请求并释放。 */
extern cJSON *edc_context_make_todo_reminder(struct edc_task *task);

/* 把本次 LLM 请求的 "system 部分" (头部) 组装并追加到 msgs 数组, 返回追加
 * 条数。此处只放会话级身份/记忆, 不含任何 todo 内容 (todo 引导一律走
 * edc_context_make_todo_reminder 塞队尾):
 *   [0] (可选) group memory 摘要消息 (## Group Memory), group->memory 非空才有
 *   [1] system prompt:
 *       - group 有 history: 以引用挂 history[0] (cJSON_AddItemReferenceToArray,
 *         不复制, msgs 释放不影响 group)
 *       - group 无 history / group 为 NULL: 需 conf 非空才以
 *         edc_context_build_system_prompt 现拼; 纯净文本同时经 *out_pristine
 *         (可 NULL) 带回, 由调用方决定是否写回 group->history (保持组内共享
 *         prompt 纯净; 调用方写回后后续请求即走引用分支)
 * 引用/副本/写回语义与迁移前的 edc_runtime_cb_send_llm 完全一致。 */
extern int edc_context_append_system_part(cJSON *msgs, struct edc_config *conf, struct edc_task_group *group,
                                          const char *workdir, sds *out_pristine);

/* todo reminder 的单一渲染源（原先在 edc_tools_todo.c/h）: 渲染 task->todos。
 * - render_body: 纯 "- [ ]/- [x]" 清单行（无包裹、无引导段），*out_completed 可 NULL；
 * - render_reminder: 完整 reminder 文本（含 <system-reminder> 包裹, 两段式骨架）。
 * 工具侧 (todowrite) 与请求队尾的 reminder 共用它 → 输出逐字节一致。返回 sds，调用方 sdsfree。 */
extern sds edc_context_render_todo_body(cJSON *items, int *out_completed);
extern sds edc_context_render_todo_reminder(cJSON *items);

#endif
