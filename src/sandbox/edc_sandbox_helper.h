#ifndef EDC_SANDBOX_HELPER_H
#define EDC_SANDBOX_HELPER_H

#include "sds.h"
#include <stdbool.h>

struct edc_config;

/*
 * The sandbox helper (see src/sandbox/sandbox_helper.c) is a tiny executable,
 * embedded in the binary as edc_sandbox_helper_blob (see
 * edc_sandbox_helper_blob.h), that applies the kernel sandbox and then execs
 * /bin/sh. These functions materialize it on disk and build its argv.
 *
 * Linux and macOS. On Windows edc_sandbox_helper_prepare() returns false and
 * the caller spawns the command directly. The helper itself dispatches to the
 * ns/landlock/seccomp layers on Linux and to Seatbelt on macOS (see
 * edc_seatbelt_sandbox.h).
 */

/* Extract the helper under <edc_home>/.cache/ and return its path (caller frees),
 * or NULL if it could not be materialized. */
sds edc_sandbox_helper_path(struct edc_config *conf);

/* Prepare the spawn file + argv to run the sandbox helper for `cmd`, using the
 * sandbox policy in `conf`. On success *out_file (caller sdsfree) and *out_args
 * (caller free) are set and true is returned; otherwise false (caller should
 * fall back to spawning /bin/sh directly). */
bool edc_sandbox_helper_prepare(struct edc_config *conf, const char *cmd, sds *out_file, char ***out_args);

#endif /* EDC_SANDBOX_HELPER_H */
