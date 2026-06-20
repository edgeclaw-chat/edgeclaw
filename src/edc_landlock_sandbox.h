#ifndef __BC_LANDLOCK_SANDBOX_H__
#define __BC_LANDLOCK_SANDBOX_H__

#include <glib.h>

gboolean edc_landlock_sandbox_apply(const gchar **ro_paths, const gchar **rw_paths);

#endif
