#ifndef __BC_TOOLS_MEMORY_H__
#define __BC_TOOLS_MEMORY_H__

struct edc_toolbox;

/* The four L2 session-memory tools (see docs/l2-session-memory.md §2.2):
 *   memory_search(query)              -> index lines of hits
 *   memory_list(offset, n)            -> paged index lines
 *   memory_get(id)                    -> one entry's full detail
 *   memory_write(title, summary, detail) -> foreground write: create an entry + a CREATED event
 * The first three are read-only; the fourth is the only task-side way to change session
 * memory.
 * Scope = the calling task's OWNING session (a cross-session task reads its own table). */
extern void edc_tools_memory_register(struct edc_toolbox *tb);

#endif
