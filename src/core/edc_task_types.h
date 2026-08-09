#ifndef __EDC_TASK_TYPES_H__
#define __EDC_TASK_TYPES_H__

/* ── Task 间依赖类型 ──────────────────────────────────────────
 * 从 edc_task.h 提取，放 core/ 以避免 shell ↔ agent 循环依赖 */

typedef enum
{
    TASK_DEP_JOIN,     /* 等待源 task 完成，将源 output 注入自己上下文 */
    TASK_DEP_PIPE,     /* 流式传输（后续扩展） */
    TASK_DEP_SEQUENCE, /* 等待源 task 完成，不注入 output（; 语义） */
} edc_task_dep_type;

#endif
