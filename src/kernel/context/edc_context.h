#ifndef __BC_CONTEXT_H__
#define __BC_CONTEXT_H__

#include "edc_config.h"
#include "sds.h"
#include <stdint.h>
#include <cJSON.h>

struct edc_task;
struct edc_task_group;

/* Base system prompt: identity template + the generated behaviour contract + project instructions + skills.
 * |workdir| is the session's bound working directory (absolute), used as the
 * identity's "## Current Running Directory"; NULL/empty falls back to the process
 * cwd. Returns a new sds (caller sdsfree). */
extern sds edc_context_build_system_prompt(struct edc_config *conf, const char *workdir);

/* The system prompt's tool section comes from the tool registry, which this package must not
 * depend on (//src/toolbox depends on //src/kernel; the reverse would be a cycle). So the
 * assembly root -- which owns both -- installs a provider once at startup, and the prompt
 * builder just calls it. NULL provider = no tool section (tests, headless builds).
 * The provider returns an owned sds ("" when the scope declares nothing). */
typedef sds (*EdcToolSectionFn)(void *ctx);
extern void edc_context_set_tool_section_provider(EdcToolSectionFn fn, void *ctx);

/* Runtime todo reminder (a fresh role=system message). The caller decides whether
 * to call it by the throttle rule: only when EDC_TODO_SYNC_REMIND_GAP requests have
 * passed without a todowrite call. The returned message is appended at the very END
 * of that request's message list (edc_llm_actor_req.tail_system), so a head-injected
 * todo state is not forgotten in a long context. Branches, chosen by task->todos:
 *   - task has a non-empty list -> status section = the list rows + per-turn sync
 *     cadence;
 *   - task non-NULL but empty/cleared -> status = "no list yet" + "build a plan
 *     with todowrite first" for multi-step tasks;
 *   - task NULL (non-task request) -> NULL (do not disturb).
 * The returned object is a fresh cJSON not attached to anything; it never touches
 * shared task/group data (task->todos is read-only). Caller adds and frees it. */
extern cJSON *edc_context_make_todo_reminder(struct edc_task *task);

/* Assemble this request's "system part" (the head) and append it to msgs, returning
 * the count appended. Only session-level identity/memory goes here -- no todo
 * content (todo guidance always goes to the tail via
 * edc_context_make_todo_reminder):
 *   [0] (optional) group memory summary message (## Group Memory), only when
 *       group->memory is non-empty;
 *   [1] system prompt:
 *       - group has history: attach history[0] by reference (no copy);
 *       - no history / group NULL: build it with edc_context_build_system_prompt
 *         (needs conf); the pristine text is returned via *out_pristine (may be
 *         NULL) so the caller can decide to write it back to group->history.
 * Reference / copy / write-back semantics match the pre-migration
 * edc_app_cb_send_llm. */
extern int edc_context_append_system_part(cJSON *msgs, struct edc_config *conf, struct edc_task_group *group,
                                          const char *workdir, sds *out_pristine);

/* Single render source for the todo reminder (was in edc_tools_todo.c/h), rendering
 * task->todos:
 * - render_body: plain "- [ ]/- [x]" rows (no wrapper, no guidance), *out_completed
 *   may be NULL;
 * - render_reminder: the full reminder text (with <system-reminder> wrapper).
 * The tool side (todowrite) and the tail reminder share it -> byte-identical output.
 * Returns sds, caller sdsfree. */
extern sds edc_context_render_todo_body(cJSON *items, int *out_completed);
extern sds edc_context_render_todo_reminder(cJSON *items);

/* The group's CURRENT context size: the "anchor + tail estimate".
 *   1) a member with a usable anchor (the last response's provider count, newer
 *      than the group's last fold and with position info) -> max(anchor + the
 *      estimate of what was appended after it);
 *   2) no usable anchor (just folded / no response yet) -> estimate from scratch
 *      what the group's next request would carry (memory + system + unfolded
 *      history + the widest running turn).
 * *out_estimated (may be NULL) says whether the result contains an estimate: false
 * when it comes purely from an anchor with a zero tail (i.e. provider truth). The
 * caller uses it to show an estimate marker -- an estimate must not pass for the
 * provider's measurement. */
extern int64_t edc_context_group_ctx_tokens(struct edc_task_group *group, bool *out_estimated);

#endif
