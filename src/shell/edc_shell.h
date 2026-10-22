#ifndef __EDC_SHELL_H__
#define __EDC_SHELL_H__

#include "edc_ast.h"
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
struct edc_msg_payload;
struct edc_task;
struct edc_task_group;

/* ── Shell execution / command handling ────────────────────── */

/* Execute a non-/-prefixed shell input (@ and plain text); true = handled. */
bool edc_shell_execute(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                       struct edc_channel_message *message);

/* One task from the whole line, verbatim -- the path edc_shell_execute takes for a plain
 * message (EDC_AST_MESSAGE). |payload| is the pass-expanded message: its text becomes
 * the task's input, its synthetic block becomes the extra message the model sees. */
bool edc_shell_spawn_plain_task(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                                struct edc_channel_message *message, const struct edc_msg_payload *payload);

/* Handle a built-in command (/whoami, /model); true = handled. */
/* The line-level builtins that live in the shell layer (`/whoami`, `/model`,
 * `/sandbox`, `/shell`). |cmd| is the parsed command line: the name plus the
 * already-unquoted argv (see struct edc_ast_cmd). */
bool edc_shell_handle_builtin(struct edc_config *cfg, struct edc_session_manager *sm,
                              struct edc_reply_to to, const struct edc_ast_cmd *cmd);

/* The predecessor of a chained task (`a | b`, `a ; b`, `a && b`, `a || b`): WHERE to wait and
 * HOW the wait behaves travel together, because a dep type without a predecessor is not a
 * thing — that pairing is what a bare `NULL, TASK_DEP_SEQUENCE` used to fake. */
struct edc_prev_dep
{
    struct edc_task *task;
    edc_task_dep_type type;
};

/* Generic task creation + group management + deps + turn + LLM dispatch. */

/* Build a task from one AST command node: translate the shell's syntax into an
 * edc_task_spec, then let the scheduler create and place it (the group/foreground/
 * turn/edge decisions live there, not here). |prev| is NULL for a task that starts a
 * chain. Does NOT reply: on error it writes the reason into *out_error (caller frees; NULL
 * when there is no error) -- task creation must not write into the channel as a side
 * effect. */
struct edc_task *edc_shell_spawn_task(struct edc_session_manager *sm, struct edc_scheduler *sched,
                                      edc_channel_type channel_type, struct edc_session *session,
                                      struct edc_cmd_task_opts *opts,
                                      const struct edc_prev_dep *prev, struct edc_task_group *target_group,
                                      const char *reply_prefix, struct edc_task_group **out_group,
                                      sds redirect_stdout, bool redirect_append, sds redirect_stderr,
                                      sds *out_error);

/* Read file contents; on failure set an error message in *err_out. */

/* ── Task references ({{…}}) ───────────────────────────────── */

sds edc_shell_collect_var_refs(struct edc_session_manager *sm, struct edc_scheduler *sched, const char *input,
                               sds **out_deps, int *out_dep_count,
                               sds *out_error);

#endif
