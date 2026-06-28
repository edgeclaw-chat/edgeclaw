#ifndef __BC_SANDBOX_H__
#define __BC_SANDBOX_H__

#include "edc_error.h"
#include <stdbool.h>

struct edc_config;

extern bool edc_sandbox_init(struct edc_config *conf);
extern const char *edc_sandbox_get_root(struct edc_config *conf);
extern char *edc_sandbox_verify_path(struct edc_config *conf, const char *path, bool writable, EdcError **error);
extern bool edc_sandbox_apply(struct edc_config *conf);

#endif
