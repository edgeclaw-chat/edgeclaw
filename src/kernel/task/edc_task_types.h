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
                        * (`;` semantics). Failure/cancel propagate exactly like JOIN --
                        * the two kinds differ in the output injection and in nothing
                        * else. */
    TASK_DEP_BARRIER,  /* an internal order barrier the runtime hangs on its own (a fold
                        * in flight, an L2 curation round): the target waits for the
                        * source, and if the source dies the edge is voided and the target
                        * proceeds -- a maintenance round failing must never take the
                        * tasks it was holding with it. Never created by a user command. */
} edc_task_dep_type;

#endif
