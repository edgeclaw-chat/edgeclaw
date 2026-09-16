#ifndef __EDC_TASK_TYPES_H__
#define __EDC_TASK_TYPES_H__

/* ── Task 间依赖类型 ──────────────────────────────────────────
 * 从 edc_task.h 提取，放 common/ 以避免 shell ↔ app 循环依赖 */

typedef enum
{
    TASK_DEP_JOIN,     /* 等待源 task 完成，将源 output 注入自己上下文；
                        * 源被取消 → 本 task 也随之取消（依赖不可满足） */
    TASK_DEP_SEQUENCE, /* 等待源 task 完成，不注入 output（; 语义）；
                        * 源被取消 → 仅抹去本条依赖，本 task 可继续 */
} edc_task_dep_type;

#endif
