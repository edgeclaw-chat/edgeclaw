#ifndef __BC_LANDLOCK_SANDBOX_H__
#define __BC_LANDLOCK_SANDBOX_H__

#include <stdbool.h>

bool edc_landlock_sandbox_apply(const char **ro_paths, const char **rw_paths);

#endif
