#ifndef __BC_COMMANDS_H__
#define __BC_COMMANDS_H__

#include "edc_channel_actor.h"
#include "edc_config.h"
#include "sds.h"
#include <stdbool.h>
#include <stddef.h>
struct edc_actor;
struct edc_channel_message;
struct edc_config;
struct edc_scheduler;
struct edc_session;
struct edc_session_manager;
struct edc_task;
struct edc_task_group;

/* The command handler (the userland entry). It takes exactly the few things its
 * downstream really needs (signature = reader roster) -- borrowing no umbrella object:
 * cfg for displays like /model, sm/sched for the task graph and store,
 * channel_actor+actor+channel_type for the "reply address". */
/* 分派表的命令名（不含斜杠），供测试做"文档 ↔ 派发"的一致性断言。返回写入条数。 */
extern int edc_commands_table_names(const char **out, int cap);

extern bool edc_commands_handler(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                                 struct edc_reply_to to, struct edc_channel_message *);
extern void edc_commands_reply(struct edc_reply_to to, const char *text);

/** The `/group info` body: one group's context and fold numbers, so "why is this group
 *  not folding yet?" is answerable without reading the snapshot trace or doing the
 *  arithmetic by hand. `group` may be NULL (nothing to describe). Exposed for tests. */
extern sds edc_commands_group_info_text(struct edc_session *session, struct edc_task_group *group,
                                        const struct edc_config *conf);

/** The command help, rendered from the command table: with |arg| NULL or empty the full
 *  list, otherwise the matching command(s)' detail. Exposed for tests (and so docs could be
 *  generated from the same table). Caller frees. */
extern sds edc_commands_help_text(const char *arg);

/* ── The command table as data (read-only) ───────────────────
 * The TUI's completion palette consumes the surface through these instead of parsing
 * `edc_commands_help_text()`'s markdown. Rows are the help lines; families are the first
 * tokens (one per row), for the bare-`/` list. All strings are borrowed (static
 * lifetime); an out-of-range index yields NULL. */
extern size_t edc_commands_doc_count(void);
extern const char *edc_commands_doc_cmd(size_t index);
extern const char *edc_commands_doc_brief(size_t index);
extern size_t edc_commands_family_count(void);
extern const char *edc_commands_family_name(size_t index);
extern const char *edc_commands_family_brief(size_t index);

/** Structured command reply: the same exit as edc_commands_reply (the same
 *  to.channel_actor), only the message type changes (TASK_HISTORY) -- the frontend then
 *  renders it as a message history rather than a text blob. |payload| is the JSON from
 *  edc_scheduler_task_history_json. */
extern void edc_commands_send_history(struct edc_reply_to to, const char *payload);

/* ── Task-creation receipt ─────────────────────────────────── */

/** The one-line receipt for a just-created task, or an EMPTY sds when there is nothing
 *  to say. The receipt only speaks about what the UI does not show -- that the task runs
 *  in the background, or what it waits for:
 *    "→ task #1 · group #1 · background"
 *    "→ task #4 · group #3 · queued behind #3"
 *    "→ task #5 · group #3 · waits for #4 (outputs injected)"
 *  A plain foreground creation ("starts now") is silent: the sidebar row and the echoed
 *  input already say it. Also empty for a system task or a NULL task. Pure (reads only the
 *  task). Exposed for tests. Caller frees. */
extern sds edc_commands_task_receipt_text(const struct edc_task *task);

/** The receipt for the task(s) ONE input created. A single task yields its own line (see
 *  above); a chain (`a | b | c`) yields one aggregated line naming them all, so a pipeline
 *  is a single receipt rather than one per segment. Empty when nothing is worth saying.
 *  Pure. Exposed for tests. Caller frees. */
extern sds edc_commands_tasks_receipt_text(struct edc_task *const *tasks, size_t count);

/** Send that receipt to the user (no-op when the text is empty). */
extern void edc_commands_announce_task(struct edc_reply_to to, const struct edc_task *task);
extern void edc_commands_announce_tasks(struct edc_reply_to to, struct edc_task *const *tasks, size_t count);

/* ── Ports: the command layer provides `handle_command` ──────
 *
 * "The provider installs its own port": the handler lives here, the config / session
 * store it needs are on the scheduler (sched->config / sched->sessions), and the reply
 * address is the sender the scheduler passes in when calling -- the assembly root no
 * longer relays it (and need not hold a function pointer). */
struct edc_scheduler; /* fwd */
extern void edc_commands_bind_scheduler(struct edc_scheduler *sched);

#endif
