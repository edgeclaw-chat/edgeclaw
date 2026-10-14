#ifndef __BC_COMMANDS_H__
#define __BC_COMMANDS_H__

#include "edc_channel_actor.h"
#include "edc_config.h"
#include "sds.h"
#include <stdbool.h>
struct edc_actor;
struct edc_channel_message;
struct edc_config;
struct edc_scheduler;
struct edc_session;
struct edc_session_manager;
struct edc_task_group;

/* The command handler (the userland entry). It takes exactly the few things its
 * downstream really needs (signature = reader roster) -- borrowing no umbrella object:
 * cfg for displays like /model, sm/sched for the task graph and store,
 * channel_actor+actor+channel_type for the "reply address". */
extern bool edc_commands_handler(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                                 struct edc_reply_to to, struct edc_channel_message *);
extern void edc_commands_reply(struct edc_reply_to to, const char *text);

/** The `/group info` body: one group's context and fold numbers, so "why is this group
 *  not folding yet?" is answerable without reading the snapshot trace or doing the
 *  arithmetic by hand. `group` may be NULL (nothing to describe). Exposed for tests. */
extern sds edc_commands_group_info_text(struct edc_session *session, struct edc_task_group *group,
                                        const struct edc_config *conf);

/** Structured command reply: the same exit as edc_commands_reply (the same
 *  to.channel_actor), only the message type changes (TASK_HISTORY) -- the frontend then
 *  renders it as a message history rather than a text blob. |payload| is the JSON from
 *  edc_scheduler_task_history_json. */
extern void edc_commands_send_history(struct edc_reply_to to, const char *payload);

/* ── Ports: the command layer provides `handle_command` ──────
 *
 * "The provider installs its own port": the handler lives here, the config / session
 * store it needs are on the scheduler (sched->config / sched->sessions), and the reply
 * address is the sender the scheduler passes in when calling -- the assembly root no
 * longer relays it (and need not hold a function pointer). */
struct edc_scheduler; /* fwd */
extern void edc_commands_bind_scheduler(struct edc_scheduler *sched);

#endif
