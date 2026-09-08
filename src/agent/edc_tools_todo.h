#ifndef __BC_TOOLS_TODO_H__
#define __BC_TOOLS_TODO_H__

#include "sds.h"
#include <cJSON.h>

struct edc_toolbox;
extern void edc_tools_todo_register(struct edc_toolbox *tb);

/* 渲染"- [ ]/- [x]" 清单行 (无 "# TODO" 头与统计尾巴)。与 render_md 共用
 * 同一行格式源，供 system prompt 注入方按需拼装。*out_completed (可 NULL)
 * 返回已完成条数。items 为 NULL/空数组 → 空 sds。调用方负责 sdsfree。 */
extern sds edc_tools_todo_render_body(cJSON *items, int *out_completed);

/* 完整 checklist markdown:
 *   # TODO
 *   - [ ] step 1
 *   - [ ] step 2 <- doing...
 *   - [x] step 3
 *
 *   1/3 completed
 * 工具回显 (update_todolist 的消息 content / tool result) 使用。
 * 返回 sds，调用方负责 sdsfree；items 为 NULL 时按空清单渲染。 */
extern sds edc_tools_todo_render_md(cJSON *items);
#endif
