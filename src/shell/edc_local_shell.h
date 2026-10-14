#ifndef __EDC_LOCAL_SHELL_H__
#define __EDC_LOCAL_SHELL_H__

#include <stdbool.h>

struct edc_channel_message;
struct edc_reply_to;
struct edc_scheduler;
struct edc_session_manager;

/* ── `!cmd`: a user-typed shell command, as a task ────────────
 *
 * `!git status` creates a task whose whole job is to issue ONE `exec_shell` call: the
 * task is seeded with that call (edc_task_spec.initial_tool_*), the ordinary tool path
 * runs it, and the result becomes the task's output and lands in the group's history.
 * No model is asked anything -- see docs/local-shell-command.zh.md.
 *
 * Handles: `!cmd`, `!!` (re-run this session's previous `!` command), the empty-`!`
 * usage reply, and the `\!` escape (which is forwarded to the ordinary input path).
 * Returns true when it took the input; false lets the caller handle it normally. */
extern bool edc_local_shell_handle(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                                   struct edc_channel_message *message);

#endif
