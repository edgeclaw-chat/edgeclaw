#ifndef __BC_TOOLS_MEMORY_H__
#define __BC_TOOLS_MEMORY_H__

struct edc_toolbox;

/* L2 session 记忆的四个工具 (见 docs/l2-session-memory.md §2.2):
 *   memory_search(query)              -> 命中条目的索引行
 *   memory_list(offset, n)            -> 翻页列举索引行
 *   memory_get(id)                    -> 一条条目的明细全文
 *   memory_write(title, summary, detail) -> 前台直写: 新建条目 + 一条 CREATED 事件
 * 前三个只读; 第四个是唯一由 task 侧改 session 记忆的入口。
 * 作用域 = 调用方 task 的**属主 session** (跨 session 任务读自己的那张表)。 */
extern void edc_tools_memory_register(struct edc_toolbox *tb);

#endif
