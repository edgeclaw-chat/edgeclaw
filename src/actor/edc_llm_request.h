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
 * throttle / config / session store) comes via sched and task; two values the kernel
 * cannot get are handed in BY PARAMETER at assembly time (no new core fields):
 *   tool_schemas -- the tool-definition array; owned by the toolbox (freed by
 *                   edc_toolbox_free), read-only here; non-NULL and sink != GROUP_MEMORY
 *                   puts it in the request body; NULL = this request carries no tools
 *                   (the fold summary call)
 *   llm_actor    -- the LLM endpoint mailbox (owned by the assembly root)
 *
 * $VAR's "value semantics" ($$ / $LAST / $N / $NAME) are a static resolver private to
 * this file, its data source = sched->sessions (the store handle). */
extern void edc_llm_request_send(struct edc_scheduler *sched, struct edc_task *task, cJSON *tool_schemas,
                                 edc_actor_t *llm_actor, void *request_id);

/* ── Ports: the request assembler provides `send_llm` ────────
 *
 * How the body is assembled and whom it is posted to is this file's knowledge, so
 * "install the port onto the scheduler" is here too. Two values are handed in once at
 * assembly time; every request then leaves from here -- the assembly root no longer
 * appears on a runtime path. Port signatures (edc_scheduler.h) are unchanged.
 *
 * |tool_schemas| is read-only, still owned by the toolbox; both values live for the
 * process. */
struct edc_scheduler; /* fwd */
extern void edc_llm_request_bind_scheduler(struct edc_scheduler *sched, edc_actor_t *llm_actor,
                                           cJSON *tool_schemas);

#endif
