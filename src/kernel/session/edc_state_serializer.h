#ifndef __EDC_STATE_SERIALIZER_H__
#define __EDC_STATE_SERIALIZER_H__

#include "edc_scheduler.h"
#include "edc_session.h"
#include <cJSON.h>
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────
 *  Pure in-memory serialize / deserialize engine.
 *
 *  edc_state_serialize()  converts a live session + scheduler
 *  into a self-contained cJSON tree (schema `version: 3`).
 *
 *  edc_state_deserialize() rebuilds all groups, tasks, turns,
 *  dependencies, and fork chains from that tree back into a
 *  (possibly fresh) session + scheduler.  ONLY `version: 3` is
 *  accepted -- the v2 read-compat path (in-memory migration) has been
 *  removed, so older archives must be migrated offline first (see
 *  docs/session-model-v3.md §10).  Any other version fails.
 *
 *  No file I/O — the caller owns the cJSON object and the
 *  rebuilt structures.
 * ───────────────────────────────────────────────────────────── */

/**
 * Schema version of a serialized tree.
 *
 * @return the `version` number, or 0 if the tree/field is missing or invalid.
 */
int edc_state_json_version(cJSON *json);

/* Current on-disk schema version (single source of truth =
 * EDC_STATE_SCHEMA_VERSION): loading accepts only this version, older data runs a
 * one-shot migration first. Tests/tools should use this, not hard-code it. */
extern int edc_state_schema_version(void);

/**
 * Serialize session + scheduler state into a cJSON tree.
 *
 * @param session   Must be non-NULL.
 * @param sched     Unused (kept for call-site compatibility); the foreground
 *                  group lives on the session now.
 * @return          cJSON object (caller owns, must cJSON_Delete).
 *                  NULL on allocation failure.
 */
cJSON *edc_state_serialize(struct edc_session *session, struct edc_scheduler *sched);

/**
 * Deserialize a cJSON tree back into live session + scheduler structures.
 *
 * All groups, tasks, turns, dependencies, and fork chains are rebuilt.
 * Accepts ONLY `version` 3; every other version fails (there is no
 * in-memory migration -- see the header note above).
 *
 * @param json      cJSON tree from edc_state_serialize().
 * @param session   Target session.
 *                  Existing groups in the session are NOT freed —
 *                  the caller should start from a fresh session.
 * @param sched     Target scheduler (used to observe task ids for the
 *                  high-water mark).  The session's foreground group is set on
 *                  the session itself, not mirrored onto the scheduler.
 *                  May be NULL if no scheduler exists yet.
 * @return          true on success, false on parse / allocation error.
 */
bool edc_state_deserialize(cJSON *json, struct edc_session *session, struct edc_scheduler *sched);

#endif /* __EDC_STATE_SERIALIZER_H__ */
