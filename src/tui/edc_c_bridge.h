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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct edc_agent;
struct edc_actor;
struct edc_channel_message;
/* render_pref 取值（与 C 侧 edc_render_pref_t 对应；此处内联以免 C++ 侧
 * include sds 系 C 头）。零值即默认 PLAIN：发送方未声明一律按纯文本显示。 */
enum
{
    EDC_C_RENDER_PLAIN = 0,
    EDC_C_RENDER_MARKDOWN = 1,
};
struct uv_loop_s;

/* ── Task-tree snapshot (parsed from the scheduler's JSON) ─────
 * The scheduler serializes the active session's task tree to JSON and the
 * TUI requests a parse through edc_c_task_snapshot_parse(); the result is a
 * plain-C tree of malloc'd strings so the C++ TUI never touches cJSON. Each
 * task node also carries the todo list it holds (the todowrite items,
 * NULL/0 when the task has none) — the TodoWidget renders the foreground
 * group's active task's todo straight from the snapshot. */

struct edc_c_todo_item
{
    const char *content;
    const char *status;   /* pending | in_progress | completed | cancelled */
    const char *priority; /* high | medium | low ("" when unset) */
};

struct edc_c_task_info
{
    const char *task_id;
    const char *state; /* scheduler state name, e.g. "COMPLETED" */
    int is_leader;
    int is_active;
    const char *description;
    int todo_count;                 /* 0 when the task holds no todo list */
    struct edc_c_todo_item *todo_items;
};

/* One conversation message of a process group, carried by a snapshot requested
 * with edc_c_agent_request_task_snapshot_history(). Projected from the group's
 * OpenAI-style history entries: a user turn, an assistant turn (content plus
 * optional tool_calls), or a tool result (name + content). The TUI maps role
 * (+has_tool_calls) onto its chat widgets; content/name are never NULL (empty
 * string when absent). */
struct edc_c_history_message
{
    const char *role;    /* "user" | "assistant" | "tool" */
    const char *content; /* message body, "" when the entry has none */
    const char *name;    /* tool name for role "tool", else "" */
    int has_tool_calls;  /* 1 when an assistant entry carries tool_calls */
    const char *task_id; /* id of the task that produced this entry ("1", ...)
                            or "" when it has no owner; lets the chat view tag
                            seeded entries with their originating task */
};

struct edc_c_task_group
{
    const char *group_id;
    int member_count;
    int msg_count; /* messages in this group: committed (no system prompt) +
                      in-flight turns of its tasks */
    int orphaned;
    int fg;
    int has_history; /* 1 when the snapshot carried this group's "messages"
                        (only the foreground group, on a history request) */
    int history_count;
    struct edc_c_history_message *history;
    int task_count;
    struct edc_c_task_info *tasks;
};

struct edc_c_task_snapshot
{
    const char *session_id;
    const char *session_name;
    int message_count;    /* messages in the session history */
    const char *created_at;  /* "YYYY-MM-DD HH:MM:SS" or "" */
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
/* Id of the task that produced this message ("1", "2", ...) or NULL when the
 * sender is not a task (user input, task snapshot, ...). Lets the chat view
 * label messages with their task. */
const char *edc_c_channel_message_task_id(struct edc_channel_message *channel_message);
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

/* Renderer preference declared by the sender (EDC_C_RENDER_PLAIN/MARKDOWN).
 * PLAIN is the default (zero value): an undeclared message renders as plain
 * text; only explicitly declared MARKDOWN content is run through markdown. */
int edc_c_channel_message_render_pref(struct edc_channel_message *channel_message);

/* Request a snapshot of the active session's task tree. The scheduler builds
 * it on its own thread and delivers it back through the channel adapter as an
 * EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE message. */
void edc_c_agent_request_task_snapshot(struct edc_agent *agent);

/* Like edc_c_agent_request_task_snapshot(), but the returned snapshot also
 * carries the foreground process group's conversation messages (see
 * edc_c_task_group::has_history/history). One-shot, meant to be used once at
 * TUI startup to refill the chat log after the agent restored a saved session;
 * ordinary polling must use the plain request so the whole history is not
 * re-serialized on every refresh. */
void edc_c_agent_request_task_snapshot_history(struct edc_agent *agent);

void edc_c_file_utils_init(struct uv_loop_s *loop);
void edc_c_agent_send_user_message(struct edc_agent *agent, const char *content);

/* Copy the path of the TUI input-history file ($EDC_HOME/.tui_history) into
 * |buf| (NUL-terminated, at most |cap| bytes). Returns true when the path fit;
 * false leaves |buf| empty. Only the path is resolved here so the C++ TUI can
 * read/write the file with the standard stream API without touching sds. */
bool edc_c_history_path(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* EDC_C_BRIDGE_H */
