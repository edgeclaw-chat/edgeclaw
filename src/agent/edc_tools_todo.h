#ifndef __BC_TOOLS_TODO_H__
#define __BC_TOOLS_TODO_H__

#include "sds.h"
#include <cJSON.h>

struct edc_toolbox;
extern void edc_tools_todo_register(struct edc_toolbox *tb);

/* 把 todo items 数组 (update_todolist 的参数, 每个 item 含
 * content/status/activeForm) 渲染成 checklist markdown:
 *   # TODO
 *   - [ ] step 1
 *   - [ ] step 2 <- doing...
 *   - [x] step 3
 *
 *   1/3 completed
 * 工具 (update_todolist 的消息 content / tool result) 与 agent 侧
 * (把 task 挂载的 todo 追加到 system prompt) 共用，保证两处文本一致。
 * 返回 sds，调用方负责 sdsfree；items 为 NULL 时按空清单渲染。 */
extern sds edc_tools_todo_render_md(cJSON *items);
#endif
