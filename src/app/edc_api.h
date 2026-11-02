#ifndef __EDC_API_H__
#define __EDC_API_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* ── The frontend's C-safe surface (`//src/app:api`, split from edc_app.c on
 * 2026-09-16) ───────────────────────────────────────────────────────────────
 *
 * This is not business logic, it is a projection: `edc_app.h` handles assembly
 * and lifetime (create / shutdown / free / hand out handles), while "project the
 * app's state into something C++ can read" is a separate concern whose only
 * reason to exist is the C++ <-> C boundary (it used to live in tui's
 * `edc_c_bridge`; moved into app by 92b2c99; now its own module). The rule: no
 * function here contains any business logic -- send an input / request a tree /
 * register an adapter / read a string.
 *
 * Same hard rule as `edc_app.h`: C++-safe -- opaque pointers only, no header that
 * pulls in sds (`sds.h`'s SDS_HDR_VAR macro does not compile as C++; cJSON/uthash
 * are fine). So sds fields become `...(char *buf, size_t cap)` readers, and
 * message fields become the read-only window (`edc_app_message_*`).
 *
 * The two handle accessors (`edc_app_config` / `edc_app_channel_actor`, in
 * `edc_app.h`) are this module's entire contract with the root: in the
 * implementation `app->` appears 0 times, and `struct edc_app` stays opaque. */
struct edc_app;
struct edc_channel_message;

/* ── Read-only window onto a channel message ──────────────────
 * The message's own fields are sds (or only complete on the C side), so the
 * frontend only sees this group: the const char* values are BORROWED, lifetime =
 * that message; everything else is int/bool. */
extern const char *edc_app_message_content(const struct edc_channel_message *message);
extern const char *edc_app_message_header(const struct edc_channel_message *message);
extern const char *edc_app_message_task_id(const struct edc_channel_message *message);
extern const char *edc_app_message_session_id(const struct edc_channel_message *message);
extern int edc_app_message_is_user(const struct edc_channel_message *message);
extern int edc_app_message_is_tool(const struct edc_channel_message *message);
extern int edc_app_message_is_thought(const struct edc_channel_message *message);
extern int edc_app_message_is_resp(const struct edc_channel_message *message);
extern int edc_app_message_is_sys(const struct edc_channel_message *message);
extern int edc_app_message_is_task_history(const struct edc_channel_message *message);
extern int edc_app_message_is_task_snapshot(const struct edc_channel_message *message);
extern int edc_app_message_is_task_tree_changed(const struct edc_channel_message *message);
extern int edc_app_message_from_active_task(const struct edc_channel_message *message);
extern int edc_app_message_render_pref(const struct edc_channel_message *message);

/* Render preference (mirrors common's edc_render_pref_t; inlined here so C++ need
 * not include the sds-family C header). Zero = PLAIN: an undeclared sender is
 * plain text. The mapping is pinned by a _Static_assert in `edc_api.c`. */
enum
{
    EDC_APP_RENDER_PLAIN = 0,
    EDC_APP_RENDER_MARKDOWN = 1,
};

/* ── Config / session string reads (sds -> caller buffer) ─────
 * Contract: on success write |buf| and return true; on failure clear |buf| and
 * return false. |cap| = total buffer bytes including the trailing NUL. */
extern bool edc_app_config_model(const struct edc_app *app, char *buf, size_t cap);
extern bool edc_app_config_api_url(const struct edc_app *app, char *buf, size_t cap);
extern bool edc_app_config_api_key(const struct edc_app *app, char *buf, size_t cap);
extern bool edc_app_sandbox_enabled(const struct edc_app *app);
/* Whether the shell-like input syntax is on (sigil + operators). ON by default;
 * /shell off turns it off, and /shell alone reports it. It is read per frame so the input
 * line can say how the current line will be read. */
extern bool edc_app_shell_syntax(const struct edc_app *app);

/* How the command layer will read the line currently being typed -- the frontend's copy
 * of the ONE predicate (edc_commands_input_kind), so a tag on the input line can never
 * disagree with what Enter does. |text| is the raw draft (not trimmed). */
typedef enum
{
    EDC_INPUT_TEXT = 0, /* one task, verbatim -- the default, and the common case */
    EDC_INPUT_COMMAND,  /* /… */
    EDC_INPUT_LOCALSHELL, /* !… (never gated by the shell switch) */
    EDC_INPUT_SHELL,    /* a shell line: quoted strings ± the sigil + */
    EDC_INPUT_REJECTED, /* the line is already an error (unterminated quote, "+!cmd", …) */
} edc_input_mode;

extern edc_input_mode edc_app_input_mode(const struct edc_app *app, const char *text);
/* First-run wizard: false until the user finished/skipped the setup page. */
extern int edc_app_onboarded(const struct edc_app *app);
/* The configured context window (tokens), for the setup page's default. */
extern int edc_app_config_context_window(const struct edc_app *app);

/* ── Model catalog (read-only projection of edc_models) ───────
 * The setup page / model picker read the embedded catalog through these. All returned
 * strings are BORROWED (lifetime = the loaded catalog); sizes are counts. */
extern size_t edc_app_models_providers(void);
extern const char *edc_app_models_provider_id(size_t index);
extern const char *edc_app_models_provider_name(const char *provider_id);
extern const char *edc_app_models_provider_api(const char *provider_id);
extern const char *edc_app_models_provider_env_name(const char *provider_id, size_t index);
extern size_t edc_app_models_count(const char *provider_id);
extern const char *edc_app_models_model_id(const char *provider_id, size_t index);
extern const char *edc_app_models_model_name(const char *model_id);
extern int edc_app_models_model_context(const char *model_id);
extern int edc_app_models_model_output(const char *model_id);
extern int edc_app_models_model_is_chat(const char *model_id);

/* ── Command surface (read-only projection of the shell's command table) ──────
 * The TUI's completion palette reads these. Static data -> no `struct edc_app`
 * parameter (same rule as edc_app_models_*). Borrowed strings. */
extern size_t edc_app_cmd_count(void);
extern const char *edc_app_cmd_text(size_t index);
extern const char *edc_app_cmd_brief(size_t index);
extern size_t edc_app_cmd_family_count(void);
extern const char *edc_app_cmd_family_name(size_t index);
extern const char *edc_app_cmd_family_brief(size_t index);

/* The shell line's option vocabulary (what may follow a quoted description), used by the
 * completion popup when the input is a shell line. Same shape as the command rows. */
extern size_t edc_app_shell_option_count(void);
extern const char *edc_app_shell_option_text(size_t index);
extern const char *edc_app_shell_option_brief(size_t index);
/* ── Prompt library (read-only projection of <workspace>/prompts) ────────────
 * The frontend's completion popup offers `/prompt <name>`, so it needs the template
 * names: one "<name>\t<description>" line per template (the description is empty when the
 * front matter has none). Returns how many templates exist; |buf| is always
 * NUL-terminated and truncated to |cap| when the list does not fit.
 *
 * |workdir| is the DISPLAYED session's bound directory (the frontend already holds it):
 * the project roots are looked for by walking up from it. Pass NULL/"" when there is no
 * session bound yet. */
extern size_t edc_app_prompts(const struct edc_app *app, const char *workdir, char *buf, size_t cap);

/* The skills, as "<name>\t<description>" lines, for `/skill`'s completion vocabulary.
 * Reads the same resource roots as everything else (|workdir| = the displayed session's
 * directory; NULL/"" = the process cwd). Returns how many skills exist. */
/* `@` completion: up to |cap| bytes of "<relative path>\t<f|d>\n" rows, best match
 * first (20 max), under |workdir|. The directory is sandbox-checked: one that cannot be
 * listed answers 0 rows. Safe to call on the UI thread -- the tree is cached after the
 * first walk. */
/* Does |path| name something the mention pass would actually read? Same resolution as the
 * pass (relative to the session's directory, through the sandbox), plus an existence check.
 * The input line colours a reference only when this says yes: the mark means "this is a
 * real reference". */
extern int edc_app_mention_exists(struct edc_app *app, const char *workdir, const char *path);

extern size_t edc_app_file_candidates(struct edc_app *app, const char *workdir, const char *query, char *buf,
                                      size_t cap);
/* The tree may have changed (a message was just submitted): walk again next time. */
extern void edc_app_file_candidates_reset(void);

extern size_t edc_app_skills(const struct edc_app *app, const char *workdir, char *buf, size_t cap);

/* The transcript path for a given session (`<workspace>/<session_id>/chat.jsonl`).
 * Built by convention, does not query the store; |session_id| must be an id (as
 * carried by snapshots/messages), not a display name. */
extern bool edc_app_chat_log_path_for(const struct edc_app *app, const char *session_id, char *buf, size_t cap);
/* The frontend's command history file (`<bc_home>/.tui_history`). */
extern bool edc_app_history_path(char *buf, size_t cap);

/* ── A few things the frontend does ─────────────────────────── */
/* Register the frontend as the only channel adapter: every on-screen message
 * reaches |on_send|. */
extern void edc_app_channel_adapter_init(struct edc_app *app, void *instance,
                                         void (*on_send)(void *, struct edc_channel_message *));
/* Send a user input; |session_id| names its session (NULL/"" = the active one).
 *
 * |payload_json| is the frontend's folded-paste map (placeholder -> real bytes, JSON
 * object) or NULL. It is applied HERE, through the text-layer passes, before anything
 * looks at the line: the content this function queues is always the finished text, so
 * classification (edc_commands_input_kind), the lexer and the {{…}} expansion all see
 * bytes that a typed line would have produced. A caller that already expanded its text
 * (the TUI does, to show the bubble) just gets a no-op -- the pass is idempotent.
 *
 * The payload itself rides along on the queued message as provenance and is stored on
 * the task's user message (fork/serialize copy it with the message). */
/* |mid| is the frontend-generated id of this user message (NULL = none). It is stored on
 * the message next to the payload; the input history remembers it, and
 * edc_app_request_message_payload() hands the payload back by that id. */
extern void edc_app_send_user_message(struct edc_app *app, const char *session_id, const char *content,
                                      const char *payload_json, const char *mid);
/* Ask for the folded-paste payload of the user message named by |mid|. The store belongs
 * to the scheduler thread, so the answer arrives as a MESSAGE_PAYLOAD channel message
 * (header = the mid, content = the payload JSON, "" = no such message). */
extern void edc_app_request_message_payload(struct edc_app *app, const char *mid);
/* That reply's mid (the header), or NULL. */
extern const char *edc_app_message_mid(const struct edc_channel_message *message);
/* True for the MESSAGE_PAYLOAD reply above. */
extern int edc_app_message_is_message_payload(const struct edc_channel_message *message);

/* Request a task tree: built on the scheduler thread, answered as a TASK_SNAPSHOT
 * message. */
extern void edc_app_request_task_snapshot(struct edc_app *app, const char *session_id);
/* Write a frontend-owned file (transcript / command history): under_workspace
 * requires the path to be under config.workspace and rejects symlink targets (see
 * edc_file_fopen_safe). */
extern bool edc_app_write_file(struct edc_app *app, const char *path, const char *data, size_t len,
                               bool append, bool under_workspace);
/* Create the parent dir; the path must be under the workspace (same rule as
 * edc_app_write_file). */
extern bool edc_app_ensure_parent_dir(struct edc_app *app, const char *path);

#ifdef __cplusplus
}
#endif

#endif
