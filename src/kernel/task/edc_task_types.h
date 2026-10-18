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
    TASK_DEP_BARRIER,  /* an ORDER constraint, and nothing else: wait for the source, and
                        * if the source ends -- however it ends -- release the target
                        * instead of taking it down. Two producers, both the runtime's own
                        * and neither a user command:
                        *   1. a maintenance round in flight (a fold, an L2 curation, a
                        *      compaction): the round failing must never take the tasks it
                        *      was holding with it;
                        *   2. the group queue (sched_chain_behind_unfinished): a group
                        *      runs one task at a time, and the tasks in that queue are
                        *      unrelated work -- the one in front dying must advance the
                        *      queue, not cancel everything behind it.
                        * The pair is ordered, not joined: no output is injected either way. */
} edc_task_dep_type;

#endif
