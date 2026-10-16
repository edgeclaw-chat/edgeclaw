#ifndef __EDC_SHELL_H__
#define __EDC_SHELL_H__

#include "edc_channel_actor.h"
#include "edc_config.h"
#include "edc_task_types.h"
#include "edc_shell_vars.h"
#include "sds.h"
#include <stdbool.h>

struct edc_session;

struct edc_config;
struct edc_scheduler;
struct edc_session_manager;
struct edc_actor;
struct edc_channel_message;
struct edc_cmd_task_opts;
struct edc_task;
struct edc_task_group;

/* ── Shell execution / command handling ────────────────────── */

/* Execute a non-/-prefixed shell input (@ and plain text); true = handled. */
bool edc_shell_execute(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                       struct edc_channel_message *message);

/* One task from the whole line, verbatim -- the path taken when the shell-like syntax
 * switch is OFF (see edc_config.shell_syntax). The caller (edc_commands_handler) owns the
 * gate; this only does the placement. true = handled. */
bool edc_shell_spawn_plain_task(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                                struct edc_channel_message *message);

/* Handle a built-in command (/whoami, /model); true = handled. */
bool edc_shell_handle_builtin(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_reply_to to,
                              struct edc_channel_message *message);

/* Generic task creation + group management + deps + turn + LLM dispatch. */
/* Build a task from one AST command node: translate the shell's syntax into an
 * edc_task_spec, then let the scheduler create and place it (the group/foreground/
 * turn/edge decisions live there, not here). Does NOT reply: on error it writes the
 * reason into *out_error (caller frees; NULL when there is no error) -- task creation
 * must not write into the channel as a side effect. */
struct edc_task *edc_shell_spawn_task(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                      edc_channel_type channel_type, struct edc_session *session,
                                      struct edc_cmd_task_opts *opts,
                                      struct edc_task *prev_task, struct edc_task_group *target_group,
                                      const char *reply_prefix, struct edc_task_group **out_group,
                                      sds redirect_stdout, bool redirect_append, sds redirect_stderr,
                                      edc_task_dep_type prev_dep_type, sds *out_error);

/* Read file contents; on failure set an error message in *err_out. */
sds edc_shell_read_file(const char *path, sds *err_out);

/* ── Task references ({{…}}) ───────────────────────────────── */

sds edc_shell_collect_var_refs(struct edc_session_manager *sm, struct edc_scheduler *sched, const char *input,
                               sds **out_deps, int *out_dep_count,
                               sds *out_error);

#endif
