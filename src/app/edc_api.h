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
/* Send a user input; |session_id| names its session (NULL/"" = the active one). */
extern void edc_app_send_user_message(struct edc_app *app, const char *session_id, const char *content);
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
