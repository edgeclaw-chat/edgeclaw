#ifndef __BC_TOOLS_FS_H__
#define __BC_TOOLS_FS_H__

#include "sds.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct edc_toolbox;
extern void edc_tools_fs_register(struct edc_toolbox *tb);

/* read_file's line window, as a pure function: which lines of |p|/|len| the model gets, and
 * the head marker that says so when it is not the whole file. |offset| is 1-based; |limit| <= 0
 * means the tool's ceiling (2000 lines, 64 KiB). Exposed because it is pure and the boundary
 * cases (last line without a newline, offset past the end, a window cut by bytes) are exactly
 * where an off-by-one hides. */
extern sds edc_fs_read_file_window(const char *p, size_t len, int64_t offset, int64_t limit);

/* Drop a leading UTF-8 BOM when |bom_is_metadata| says the platform treats one as metadata,
 * setting |had_bom| when it did (so a rewrite can restore it). Takes the platform's answer as
 * a parameter because edc_platform_bom_is_metadata() is a compile-time constant: without it
 * the Windows branch cannot be tested on a POSIX host, and that branch is where edit_file
 * silently dropped BOMs. |content| is owned by the caller; it is freed only when stripped. */
extern sds edc_fs_strip_bom_for_tool(sds content, bool bom_is_metadata, bool *had_bom);

#endif
