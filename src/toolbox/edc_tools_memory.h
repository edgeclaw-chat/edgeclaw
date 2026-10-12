#ifndef __BC_TOOLS_MEMORY_H__
#define __BC_TOOLS_MEMORY_H__

struct edc_toolbox;

/* The session-memory tools:
 *   memory_search(query)                 -> index lines of hits
 *   memory_list(offset, n)               -> paged index lines
 *   memory_get(id)                       -> one entry's full detail
 *   memory_write(title, summary, detail) -> create a NEW entry + a CREATED event
 *   memory_update(id, title?, summary?, detail?) -> merge into an entry + an UPDATED event
 *   memory_supersede(ids[])              -> retire entries (tombstone) + a SUPERSEDED event each; atomic
 * Every task (foreground and the curation task) uses the same set. Search first; only
 * create when nothing related exists. Nothing is ever deleted.
 * Scope = the calling task's OWNING session (a cross-session task reads its own table). */
extern void edc_tools_memory_register(struct edc_toolbox *tb);

#endif
