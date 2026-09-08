#ifndef __BC_TOOLS_TODO_H__
#define __BC_TOOLS_TODO_H__

#include "sds.h"
#include <cJSON.h>

struct edc_toolbox;
extern void edc_tools_todo_register(struct edc_toolbox *tb);

/* 渲染"- [ ]/- [x]" 清单行 (无 "<system-reminder>" 包裹与引导段)。与
 * render_reminder 共用同一行格式源——它是 todo reminder 状态段的正文。
 * *out_completed (可 NULL) 返回已完成条数。items 为 NULL/空数组 → 空 sds。
 * 调用方负责 sdsfree。 */
extern sds edc_tools_todo_render_body(cJSON *items, int *out_completed);

/* 完整 todo reminder 文本 (两段式骨架 + <system-reminder> 包裹)——update_todolist
 * 工具 result/回显 与 LLM 请求队尾 reminder (edc_context_make_todo_reminder
 * 的 content) 的【单一渲染源】, 两者逐字节一致:
 *   <system-reminder>
 *   ## 🚨 CURRENT TASK SYSTEM STATE
 *   {清单行 | No todo list exists for this task yet.}
 *
 *   ## 🛠️ INSTRUCTIONS FOR THIS TURN
 *   {逐轮同步节拍 | 建计划引导}
 *   </system-reminder>
 * items 非空 → 状态段 = 清单行 + 逐轮同步节拍; items 为 NULL/空数组 →
 * "尚无清单" + 建计划引导 (两分支互斥出现)。
 * 返回 sds，调用方负责 sdsfree。 */
extern sds edc_tools_todo_render_reminder(cJSON *items);
#endif
