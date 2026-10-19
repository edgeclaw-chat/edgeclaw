#ifndef __EDC_TASK_TYPES_H__
#define __EDC_TASK_TYPES_H__

/* ── Task dependency types ────────────────────────────────────
 * Extracted from edc_task.h into common/ to avoid a shell <-> app cycle. */

typedef enum
{
    TASK_DEP_JOIN,     /* wait for the source task to finish, then inject its output
                        * into this task's context; source cancelled -> this task is
                        * cancelled too (the dependency is unsatisfiable) */
    TASK_DEP_SEQUENCE, /* wait for the source task to finish, no output injection, and let
                        * the source's failure take this task down with it (the edge stays,
                        * so a resume returns it to dep-wait).
                        *
                        * NO PRODUCER TODAY: this is the `&&` kind -- "run the next one only
                        * if this one succeeded" -- and the grammar has no `&&` yet. `;` used
                        * to be it, and no longer is (`;` is a BARRIER: shell's `;` runs the
                        * next command whatever happened). Two things still reach this value:
                        * a saved state file from before that change (an old `;` edge loads as
                        * SEQUENCE and keeps the strict behaviour it was written with), and a
                        * future `&&`. Do not use it for ordering a group's queue. */
    TASK_DEP_BARRIER,  /* an ORDER constraint, and nothing else: wait for the source, and
                        * if the source ends -- however it ends -- release the target
                        * instead of taking it down. Never created by a user command; three
                        * producers, all the runtime's own:
                        *   1. a maintenance round in flight (a fold, an L2 curation, a
                        *      compaction): the round failing must never take the tasks it
                        *      was holding with it;
                        *   2. the group queue (sched_chain_behind_unfinished): a group
                        *      runs one task at a time, and the tasks in that queue are
                        *      unrelated work -- the one in front dying must advance the
                        *      queue, not cancel everything behind it;
                        *   3. `;` in a shell chain (edc_interp): "when this one is done,
                        *      whoever it went, run the next". `&&` would be a SEQUENCE.
                        * The pair is ordered, not joined: no output is injected either way. */
    TASK_DEP_UNLESS,   /* the INVERSE order constraint (`||`): wait for the source, then run only
                        * if the source did NOT succeed. A source that completed means this
                        * branch is not taken -- the dependent is cancelled ("skipped", end =
                        * SKIPPED) and is NOT resumable: the edge stays completed only so the
                        * record of why it never ran survives. A source that failed / was
                        * cancelled / was interrupted is exactly the case the dependent exists
                        * for: void the edge and release it. Never injected. */
} edc_task_dep_type;

#endif
