/*
 * edc_sandbox_helper.c for Windows.
 *
 * There is no kernel sandbox to apply on Windows -- edc_platform.c reports
 * kernel_sandbox = false there -- so the helper executable (sandbox_helper.c,
 * Landlock/seccomp/Seatbelt) is not built at all for this platform. The functions still
 * have to exist, because edc_tools_shell.c links against them; the header documents exactly
 * this contract: prepare() returns false and the caller spawns the command directly.
 *
 * Kept as a separate file rather than an #ifdef inside the real implementation so that the
 * POSIX file stays free of Windows branches, and the stub cannot grow logic by accident.
 */

#include "edc_sandbox_helper.h"

#include <stdlib.h>

sds edc_sandbox_helper_path(struct edc_config *conf)
{
    (void)conf;
    return NULL;
}

bool edc_sandbox_helper_prepare(struct edc_config *conf, const char *cmd, sds *out_file, char ***out_args)
{
    (void)conf;
    (void)cmd;
    (void)out_file;
    (void)out_args;
    return false;
}
