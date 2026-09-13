#ifndef EDC_C_BRIDGE_H
#define EDC_C_BRIDGE_H

/*
 * Thin C interface between the TUI (C++) and the rest of the project (C): the
 * project's headers pull in sds.h/cJSON/uthash, which do not compile under C++,
 * so the TUI never includes them -- signatures use plain C types only and the
 * implementation (edc_c_bridge.c, compiled as C) includes whatever it needs.
 * Add new C-side capabilities here, prefixed edc_c_.
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
 * edc_c_task_snapshot_parse() turns the scheduler's JSON into a plain-C tree of
 * malloc'd strings, so the C++ TUI never touches cJSON. Each task node carries the
 * todo list it holds (the todowrite items; NULL/0 when it has none). */

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
    int todo_count; /* 0 when the task holds no todo list */
    struct edc_c_todo_item *todo_items;
    /* Accumulated LLM token usage for this task (0 when none reported yet). */
    long long tokens_prompt;
    long long tokens_completion;
    long long tokens_total;
    long long tokens_cached;
};

struct edc_c_task_group
{
    const char *group_id;
    int member_count;
    int msg_count; /* messages in this group: committed (no system prompt) +
                      in-flight turns of its tasks */
    int orphaned;
    int fg;
    int task_count;
    long long tokens_total; /* sum of member tasks' total tokens */
    struct edc_c_task_info *tasks;
};

struct edc_c_task_snapshot
{
    const char *session_id;
    const char *session_name;
    int message_count;      /* messages in the session history */
    const char *created_at; /* "YYYY-MM-DD HH:MM:SS" or "" */
    int group_count;
    struct edc_c_task_group *groups;
};

/* Parse a task-tree snapshot JSON string into a malloc'd structure tree.
 * Returns NULL on parse failure. Free with edc_c_task_snapshot_free(). */
struct edc_c_task_snapshot *edc_c_task_snapshot_parse(const char *json);
void edc_c_task_snapshot_free(struct edc_c_task_snapshot *snap);

/* Create / destroy an agent. The returned pointer is opaque to the TUI; it is
 * only passed back into other bridge functions. Mirrors edc_agent_new/free.
 * |sandbox_override|: -1 = keep the persisted config.json value, 0 = force off,
 * 1 = force on (per-run only; not persisted). */
struct edc_agent *edc_c_agent_new(bool setup, int sandbox_override);
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
/* Id of the session that owns the producing task, or NULL when the sender is
 * not a task. Lets the chat view append a background session's output to that
 * session's own transcript instead of the currently displayed one. */
const char *edc_c_channel_message_session_id(struct edc_channel_message *channel_message);
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
 * EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE message. The snapshot carries only
 * sidebar data (task tree + todos + counts) — never chat text, which the TUI
 * records itself (see edc_chat_transcript.hpp). */
void edc_c_agent_request_task_snapshot(struct edc_agent *agent);

void edc_c_file_utils_init(struct uv_loop_s *loop);
void edc_c_agent_send_user_message(struct edc_agent *agent, const char *content);

/* Copy the configured LLM model name (config->llm_api_conf.model) into |buf|
 * (NUL-terminated, at most |cap| bytes). Returns true when a model is
 * configured and fit the buffer; false leaves |buf| empty. Lets the TUI show
 * the active model without pulling in the sds-based config header. */
bool edc_c_agent_model(struct edc_agent *agent, char *buf, size_t cap);

/* Same contract as edc_c_agent_model, for the configured API URL / API key.
 * Used to prefill the /connect dialog with the current values. */
bool edc_c_agent_api_url(struct edc_agent *agent, char *buf, size_t cap);
bool edc_c_agent_api_key(struct edc_agent *agent, char *buf, size_t cap);

/* True when the filesystem sandbox is currently enabled (toggled at runtime
 * with /sandbox). Lets the TUI show the sandbox state in the footer. */
bool edc_c_agent_sandbox_enabled(struct edc_agent *agent);

/* Apply new LLM provider settings at runtime: persist them to config.json and
 * hot-update the running agent (config + llm_actor). Any argument may be NULL
 * or empty to leave that field unchanged. Returns true on success. */
bool edc_c_agent_connect(struct edc_agent *agent, const char *api_url, const char *model, const char *api_key);

/* Copy the path of the TUI input-history file ($EDC_HOME/.tui_history) into
 * |buf| (NUL-terminated, at most |cap| bytes). Returns true when the path fit;
 * false leaves |buf| empty. Only the path is resolved here so the C++ TUI can
 * read/write the file with the standard stream API without touching sds. */
bool edc_c_history_path(char *buf, size_t cap);

/* ── Chat log ──────────────────────────────────────────────────────────────
 * The chat log is the TUI's own transcript, not a view derived from session.json
 * (see edc_chat_transcript.hpp). Only the file's location is resolved here. */
/* Copy the chat-transcript path (<session_history_dir>/chat.jsonl) of |session_id|
 * into |buf|. The TUI owns that file: it appends one line per bubble it shows and
 * replays the file on the next start (see edc_chat_transcript.hpp). Returns false
 * when the session is unknown or |buf| is too small. */
bool edc_c_session_chat_log_path_for(struct edc_agent *agent, const char *session_id, char *buf, size_t cap);

/* Create the parent directory of |path| (a file under the workspace) when it does
 * not exist yet. A session's directory only appears once the session is saved,
 * so the transcript of a session that has not been saved still needs one. */
bool edc_c_agent_ensure_parent_dir(struct edc_agent *agent, const char *path);

/* Write |data| to |path| with symlink safety: the final component must not be a
 * symlink (opened with O_NOFOLLOW), and when |under_workspace| is true the
 * resolved parent directory must live inside the agent workspace. |append|
 * selects append vs truncate. Returns true on success. Used by the TUI for the
 * chat transcript and the input history, whose paths can sit next to
 * model-writable files. */
bool edc_c_agent_write_file(struct edc_agent *agent, const char *path, const char *data, size_t len, bool append,
                            bool under_workspace);

/* Copy the active session's id into |buf| (NUL-terminated, at most |cap|
 * bytes). Returns true when it fit; false leaves |buf| empty. Used to key the
 * chat transcript to the session that is active at startup, before the first
 * task snapshot arrives. */
bool edc_c_agent_session_id(struct edc_agent *agent, char *buf, size_t cap);

/* Copy the active session's bound working directory into |buf| (NUL-terminated,
 * at most |cap| bytes). Returns true when the session is bound to a directory
 * and it fit; false (buf left empty) for an unbound session or on failure — the
 * caller then falls back to the process working directory. Mirrors the {{cwd}}
 * the model is told about, so the footer reflects the session actually shown. */
bool edc_c_session_workdir(struct edc_agent *agent, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* EDC_C_BRIDGE_H */
