#ifndef __BC_SANDBOX_H__
#define __BC_SANDBOX_H__

#include "edc_error.h"
#include "sds.h"
#include <stdbool.h>
#include <stddef.h>

struct edc_config;

extern bool edc_sandbox_init(struct edc_config *conf);
extern const char *edc_sandbox_get_root(struct edc_config *conf);

/* Every deny entry is matched through realpath(), so an entry that does not resolve
 * matches nothing: the directory it names is NOT masked, and without this check nothing
 * would say so. Logs one warning per bad entry and returns how many there were.
 *
 * Only denied paths are reported. An allow entry that does not resolve fails closed --
 * the path is simply not allowed -- so a typo there cannot open anything.
 * Called from edc_sandbox_init(); returns 0 when the sandbox is off or there are none. */
extern size_t edc_sandbox_report_unresolved_paths(struct edc_config *conf);
extern sds edc_sandbox_verify_path(struct edc_config *conf, const char *path, bool writable, EdcError **error);
/* Like edc_sandbox_verify_path, but a relative |path| is resolved against
 * |base| (the session's bound working directory) instead of the process cwd.
 * |base| NULL/empty, or an absolute |path|, behaves exactly like
 * edc_sandbox_verify_path. */
extern sds edc_sandbox_verify_path_at(struct edc_config *conf, const char *base, const char *path, bool writable,
                                      EdcError **error);

#endif
