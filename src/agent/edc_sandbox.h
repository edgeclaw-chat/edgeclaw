#ifndef __BC_SANDBOX_H__
#define __BC_SANDBOX_H__

#include "edc_error.h"
#include "sds.h"
#include <stdbool.h>

struct edc_config;

extern bool edc_sandbox_init(struct edc_config *conf);
extern const char *edc_sandbox_get_root(struct edc_config *conf);
extern sds edc_sandbox_verify_path(struct edc_config *conf, const char *path, bool writable, EdcError **error);
/* 对当前进程施加内核隔离 (Landlock→NS→seccomp)。仅应于 fork 后的单线程
 * 子进程、exec 之前调用; 目前未接线, 见 .c 内决策注释。 */
extern bool edc_sandbox_apply(struct edc_config *conf);

#endif
