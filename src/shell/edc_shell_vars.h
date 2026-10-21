#ifndef __EDC_SHELL_VARS_H__
#define __EDC_SHELL_VARS_H__

#include "sds.h"
#include <stdbool.h>

/* ── Task-reference primitives (leaf: depends only on common, does not know app) ──
 *
 * This layer is deliberately kept apart from the shell executor (edc_shell.c /
 * edc_commands.c, which depend on //src/app:app): the app side (edc_scheduler's /task
 * parsing, edc_app's variable expansion) needs these two entries but must never depend
 * back on the shell executor, or app <-> shell becomes a cycle. See
 * //src/shell:shell_parse. */

/* When expanding a {{…}} this callback asks the caller for the variable's value.
 * name: the variable name (e.g. "LAST", "$")
 * user_data: the caller's context
 * Returns the value (sds), or NULL on failure; the caller sdsfree()s it. */
typedef sds (*edc_shell_var_resolver_fn)(const char *name, void *user_data);

/* Expand {{…}} references (the expand phase, called once deps are done).
 * resolver: the callback that resolves variable values
 * Returns the expanded string, or NULL and sets *out_error on failure. */
sds edc_shell_expand_vars(const char *input, edc_shell_var_resolver_fn resolver, void *user_data, sds *out_error);

#endif
