#ifndef EDC_CRONTAB_JOBS_H
#define EDC_CRONTAB_JOBS_H

/* Pieces of the scheduled-job format that more than one layer needs.
 *
 * The jobs themselves live in <workspace>/jobs.json and are owned by the crontab tool;
 * this header only holds what the command layer (`/jobs`) and the ticker must agree on
 * -- currently the text a fired job hands to the input pipeline. Keeping it in one place
 * is the point: `/jobs run` must produce exactly what a timer-fired job produces, or the
 * same schedule would behave differently depending on how it was started. */
#include "sds.h"

/* The user-visible text of a fired job: the task input the pipeline sees. Caller frees. */
sds edc_crontab_task_message(const char *name, const char *message);

#endif
