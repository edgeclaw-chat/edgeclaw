#ifndef __EDC_TASK_TYPES_H__
#define __EDC_TASK_TYPES_H__

/* ── Task dependency types ────────────────────────────────────
 * Extracted from edc_task.h into common/ to avoid a shell <-> app cycle. */

typedef enum
{
    TASK_DEP_JOIN,     /* wait for the source task to finish, then inject its output
                        * into this task's context; source cancelled -> this task is
                        * cancelled too (the dependency is unsatisfiable) */
    TASK_DEP_SEQUENCE, /* wait for the source task to finish, no output injection
                        * (`;` semantics); source cancelled -> only this edge is
                        * dropped, this task continues */
} edc_task_dep_type;

#endif
