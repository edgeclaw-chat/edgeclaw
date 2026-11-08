#ifndef __EDC_LLM_REQUEST_H__
#define __EDC_LLM_REQUEST_H__

#include "edc_actor.h" /* edc_actor_t */
#include <cJSON.h>

struct edc_scheduler;
struct edc_task;

/* Assemble one LLM request and post it to the llm_actor mailbox (moved from
 * src/app/edc_app.c).
 *
 * Everything kernel-owned (group history / fold watermark / deps / active_turn / todo
 * throttle / config / session store) comes via sched and task; the tool-definition
 * arrays and the LLM mailbox are handed in BY PARAMETER at assembly time (no new core
 * fields). Which array a request carries is decided by the task's sink:
 *   tool_schemas   -- the FULL tool-definition array; a normal task carries it;
 *   memory_schemas -- only the memory_* tools; a SINK_SESSION_MEMORY (curation) task
 *                     carries it (it must be able to search/read/update memory, and
 *                     nothing else);
 *   a SINK_GROUP_MEMORY (fold) task carries neither.
 * Both arrays are read-only here and owned by the toolbox (freed by edc_toolbox_free).
 *   llm_actor    -- the LLM endpoint mailbox (owned by the assembly root)
 *
 * Task references' "value semantics" ({{<id>}} only) are a static resolver private to
 * this file, its data source = sched->sessions (the store handle). */
extern void edc_llm_request_send(struct edc_scheduler *sched, struct edc_task *task, cJSON *tool_schemas,
                                 cJSON *memory_schemas, edc_actor_t *llm_actor, void *request_id);

/* ── Ports: the request assembler provides `send_llm` ────────
 *
 * How the body is assembled and whom it is posted to is this file's knowledge, so
 * "install the port onto the scheduler" is here too. Two values are handed in once at
 * assembly time; every request then leaves from here -- the assembly root no longer
 * appears on a runtime path. Port signatures (edc_scheduler.h) are unchanged.
 *
 * |tool_schemas| / |memory_schemas| are read-only, still owned by the toolbox; all
 * three values live for the process. */
struct edc_scheduler; /* fwd */
struct edc_task;
extern void edc_llm_request_bind_scheduler(struct edc_scheduler *sched, edc_actor_t *llm_actor,
                                           cJSON *tool_schemas, cJSON *memory_schemas);

/* The per-group declaration set. A normal reply declares the GROUP's tools, not the
 * process's: one conversation must send the same `tools` bytes every turn, or the provider's
 * cached prefix is lost. The resolver lives in the assembly root because it needs the
 * registry, which this package must not depend on.
 * Returns an array owned by the group (NOT by the request); NULL = fall back to the
 * process-wide set. */
typedef cJSON *(*EdcGroupToolsFn)(void *ctx, struct edc_task *task);
extern void edc_llm_request_bind_group_tools(EdcGroupToolsFn fn, void *ctx);

#endif
