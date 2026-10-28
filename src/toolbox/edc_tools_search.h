#ifndef __BC_TOOLS_SEARCH_H__
#define __BC_TOOLS_SEARCH_H__

#include <stdint.h>
#include <stdbool.h>

struct edc_toolbox;
extern void edc_tools_search_register(struct edc_toolbox *tb);

/* The glob matcher the two tools filter with (`*` and `?`; `*` does not cross `/`).
 * Exposed because it is pure and easy to get subtly wrong -- see tests/test_toolset.c.
 * Case folding follows the platform: Windows folds, POSIX does not. */
extern bool edc_search_glob_match(const char *glob, const char *s);

#include "sds.h"
/* Run a search synchronously on the calling thread (the tool path calls this from the
 * thread pool; a test calls it directly). Returns the result text, never NULL. */
extern sds edc_search_run_blocking(const char *root, bool is_grep, const char *pattern, const char *glob,
                                   bool ignore_case, bool literal, int64_t limit);

#endif
