#ifndef EDC_C_BRIDGE_H
#define EDC_C_BRIDGE_H

/*
 * Thin C interface between the TUI (C++) and the rest of the project (C).
 *
 * The project's C headers pull in sds.h, cJSON, uthash -- C headers that
 * don't compile under C++ (sds uses void* -> struct* implicit conversions).
 * Rather than patch third-party C headers, the TUI talks to the project only
 * through this bridge: signatures use plain C types (const char*, size_t,
 * bool), never sds/cJSON. The bridge is implemented in edc_c_bridge.c (compiled
 * as C, so it can include sds-based headers freely).
 *
 * Add a function here when the TUI needs a new C-side capability, and
 * implement it in edc_c_bridge.c. This is a general C API surface, not
 * agent-specific: prefix all functions with edc_c_.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct edc_agent;
struct edc_actor;
struct edc_channel_message;
struct uv_loop_s;

/* ── Task-tree snapshot (parsed from the scheduler's JSON) ─────
 * The scheduler serializes the active session's task tree to JSON and the
 * TUI requests a parse through edc_c_task_snapshot_parse(); the result is a
 * plain-C tree of malloc'd strings so the C++ TUI never touches cJSON. */

struct edc_c_task_info
{
    const char *task_id;
    const char *state; /* scheduler state name, e.g. "COMPLETED" */
    int is_leader;
    int is_active;
    const char *description;
};

struct edc_c_task_group
{
    const char *group_id;
    int member_count;
    int orphaned;
    int fg;
    int task_count;
    struct edc_c_task_info *tasks;
};

struct edc_c_task_snapshot
{
    const char *session_id;
    const char *session_name;
    int group_count;
    struct edc_c_task_group *groups;
};

/* Parse a task-tree snapshot JSON string into a malloc'd structure tree.
 * Returns NULL on parse failure. Free with edc_c_task_snapshot_free(). */
struct edc_c_task_snapshot *edc_c_task_snapshot_parse(const char *json);
void edc_c_task_snapshot_free(struct edc_c_task_snapshot *snap);

/* Create / destroy an agent. The returned pointer is opaque to the TUI; it is
 * only passed back into other bridge functions. Mirrors edc_agent_new/free. */
struct edc_agent *edc_c_agent_new(bool setup, bool enable_sandbox);
void edc_c_agent_free(struct edc_agent *agent);

/* Register the shell command handler (edc_commands_handler) with the agent so
 * slash commands (e.g. /task, /session) typed into the TUI are handled like in
 * the CLI. Must be called after the agent exists and before sending messages. */
void edc_c_agent_set_command_handler(void);

/* The agent's channel actor, so the TUI can register itself as an adapter
 * channel (see edc_channel_adapter_init). Opaque pointer -- the TUI only
 * passes it back into C functions that take struct edc_actor*. */
struct edc_actor *edc_c_agent_channel_actor(struct edc_agent *agent);
const char *edc_c_channel_message_content(struct edc_channel_message *);
/* Optional short description carried by a message (e.g. the executed command
 * for tool messages). May return NULL when the sender provided no header. */
const char *edc_c_channel_message_header(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_user(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_tool(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_thought(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_resp(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_sys(struct edc_channel_message *channel_message);
/* True when a message carries a serialized task-tree snapshot (see
 * edc_c_agent_request_task_snapshot). Such messages must NOT be rendered as
 * chat text; the TUI parses edc_c_channel_message_content() to refresh its
 * task sidebar. */
int edc_c_channel_message_is_task_snapshot(struct edc_channel_message *channel_message);

/* Request a snapshot of the active session's task tree. The scheduler builds
 * it on its own thread and delivers it back through the channel adapter as an
 * EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE message. */
void edc_c_agent_request_task_snapshot(struct edc_agent *agent);

void edc_c_file_utils_init(struct uv_loop_s *loop);
void edc_c_agent_send_user_message(struct edc_agent *agent, const char *content);

#ifdef __cplusplus
}
#endif

#endif /* EDC_C_BRIDGE_H */
