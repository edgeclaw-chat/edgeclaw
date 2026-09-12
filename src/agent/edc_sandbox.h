#ifndef __BC_SANDBOX_H__
#define __BC_SANDBOX_H__

#include "edc_error.h"
#include "sds.h"
#include <stdbool.h>

struct edc_config;

extern bool edc_sandbox_init(struct edc_config *conf);
extern const char *edc_sandbox_get_root(struct edc_config *conf);
extern sds edc_sandbox_verify_path(struct edc_config *conf, const char *path, bool writable, EdcError **error);
/* Like edc_sandbox_verify_path, but a relative |path| is resolved against
 * |base| (the session's bound working directory) instead of the process cwd.
 * |base| NULL/empty, or an absolute |path|, behaves exactly like
 * edc_sandbox_verify_path. */
extern sds edc_sandbox_verify_path_at(struct edc_config *conf, const char *base, const char *path, bool writable,
                                      EdcError **error);
/* Apply all kernel isolation layers to the current process (NS → Landlock →
 * seccomp). Reserved for a confined single-threaded child; currently unused --
 * the active path is the sandbox helper (src/agent/sandbox_helper.c), which
 * applies the same sequence itself. See the decision note in edc_sandbox.c. */
extern bool edc_sandbox_apply(struct edc_config *conf);

#endif
