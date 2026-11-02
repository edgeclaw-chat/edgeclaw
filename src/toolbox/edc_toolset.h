#include "sds.h"
#ifndef __BC_TOOLSET_H__
#define __BC_TOOLSET_H__

#include "cJSON.h"
#include "edc_tools.h"
#include <stdbool.h>

/* The declaration set: WHICH tools the model is told about, derived from the registry plus
 * whatever the transcript says changed. Port of pi's `resolveTranscriptTools` /
 * `hasNonAdditiveToolChanges` (packages/ai/src/utils/transcript.ts:200-237); the reasoning
 * and the batch plan are in docs/pi-toolset-plan.zh.md.
 *
 * The rules, in one place:
 *   - The set is NOT a permission gate. Execution asks a different question (scope check in
 *     edc_toolbox_exec); this file only decides what the model can see.
 *   - The order is always by tool name, byte for byte, because the request's `tools` field
 *     sits in the provider's cached prefix: an unstable order would miss the cache on every
 *     turn for no reason.
 *   - A pure addition (a tool nobody had) keeps the field frozen -- the model learns about it
 *     from the delta message instead. A removal or a same-name redefinition is what forces
 *     the field to be recomputed, and that costs one cache miss (accepted, logged). */

struct edc_toolbox;
struct edc_task_group;

/* The starting declaration set: DIRECT tools whose scope matches |scope|, sorted by name.
 * Caller owns the returned array. */
extern cJSON *edc_toolset_initial(struct edc_toolbox *tb, uint32_t scope);

/* Apply every delta message in |history| to a copy of |initial| and return the current set
 * (sorted by name). Deltas are system messages carrying:
 *   "tools_added":   [ <request-shaped schema entry> , ... ]
 *   "tools_removed": [ { "name": "<tool>" } , ... ]
 * *has_non_additive is set when the transcript contains a removal or a same-name
 * redeclaration -- the condition under which the request must send THIS set instead of the
 * frozen initial one. Pass NULL when the caller does not care. */
extern cJSON *edc_toolset_replay(cJSON *initial, cJSON *history, bool *has_non_additive);

/* initial() + replay(): the set a request built from |history| would declare. */
extern cJSON *edc_toolset_resolve(struct edc_toolbox *tb, cJSON *history, uint32_t scope);

/* The system prompt's tool section (the "declaration prose"): one "- name: snippet" line per
 * declared tool, then the rules that no tool's own JSON schema can carry ("use exec_shell for
 * searching while there is no grep"), then each tool's behavioural notes.
 *
 * Deliberately terse and deliberately NOT a copy of the schemas: parameters live in the
 * request's `tools` field, and repeating them here is the drift that this design exists to
 * remove. Rendered from the same registry, in name order, so two calls on one registry are
 * byte-identical (the section sits in the cached system prompt). Empty string when the scope
 * declares nothing. Caller owns the result. */
extern sds edc_toolset_render_prompt(struct edc_toolbox *tb, uint32_t scope, cJSON *decl);

/* Record that the declaration set changed, and make it so:
 *   - a "Tool set changed (<reason>)" system message is appended to the group (kept in
 *     group->tool_deltas so a history rebuild cannot lose it, and in history so the next
 *     request already sees it);
 *   - the group's frozen declaration set is recomputed. A pure addition leaves the request's
 *     `tools` field byte-identical (the notice is how the model learns); a removal or a
 *     redefinition changes it, which costs one cache miss -- counted in toolset_epoch and
 *     logged, because "why did my bill jump" must be answerable.
 * Returns false when there was nothing to record. |removed_names| may be NULL. */
extern bool edc_toolset_note_change(struct edc_toolbox *tb, struct edc_task_group *group, cJSON *added_schemas,
                                    const char **removed_names, size_t n_removed, const char *reason);

/* The human-readable tool reference used to be a generated file (TOOLS.md). It is gone:
 * an artifact in the user's own writable workspace invites edits that the next startup
 * silently overwrites, and nothing read it once it left BOOTSTRAP_FILES. The one catalog is
 * the request's `tools` field. */

#endif
