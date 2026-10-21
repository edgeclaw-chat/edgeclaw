#include "edc_crontab_jobs.h"

sds edc_crontab_task_message(const char *name, const char *message)
{
    return sdscatprintf(sdsempty(),
                        "[Scheduled Task] Timer finished.\n\n"
                        "Task '%s' has been triggered.\n"
                        "Scheduled instruction: %s",
                        name ? name : "", message ? message : "");
}
