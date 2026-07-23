#ifndef __EDC_STATE_SERIALIZER_H__
#define __EDC_STATE_SERIALIZER_H__

#include "edc_scheduler.h"
#include "edc_session.h"
#include <cJSON.h>
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────
 *  Phase 1: Pure in-memory serialize / deserialize engine.
 *
 *  edc_state_serialize()  converts a live session + scheduler
 *  into a self-contained cJSON tree.
 *
 *  edc_state_deserialize() rebuilds all groups, tasks, turns,
 *  dependencies, and fork chains from that tree back into a
 *  (possibly fresh) session + scheduler.
 *
 *  No file I/O — the caller owns the cJSON object and the
 *  rebuilt structures.
 *
 *  Turn messages are referenced by index into session->messages
 *  (the cJSON Array loaded from history.jsonl).
 * ───────────────────────────────────────────────────────────── */

/**
 * Serialize session + scheduler state into a cJSON tree.
 *
 * @param session   Must be non-NULL.  session->messages must be populated.
 * @param sched     May be NULL (foreground_group is then omitted).
 * @return          cJSON object (caller owns, must cJSON_Delete).
 *                  NULL on allocation failure.
 */
cJSON *edc_state_serialize(struct edc_session *session, struct edc_scheduler *sched);

/**
 * Deserialize a cJSON tree back into live session + scheduler structures.
 *
 * All groups, tasks, turns, dependencies, and fork chains are rebuilt.
 *
 * @param json      cJSON tree from edc_state_serialize().
 * @param session   Target session.  session->messages MUST already
 *                  contain the messages array (loaded from history.jsonl).
 *                  Existing groups in the session are NOT freed —
 *                  the caller should start from a fresh session.
 * @param sched     Target scheduler.  foreground_group will be set.
 *                  May be NULL if no scheduler exists yet.
 * @return          true on success, false on parse / allocation error.
 */
bool edc_state_deserialize(cJSON *json, struct edc_session *session, struct edc_scheduler *sched);

#endif /* __EDC_STATE_SERIALIZER_H__ */
