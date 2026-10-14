/* The frontend's C-safe surface -- split out of `edc_app.c` (2026-09-16).
 *
 * These functions are NOT the root's business: they are the projection onto the
 * C++ <-> C boundary --
 *   - `edc_app_message_*`: message fields are sds and C++ cannot include those
 *     headers, so the read-only window lives here;
 *   - string readers: same reason (sds -> caller buffer);
 *   - a few things the frontend does: send input / request a snapshot / register
 *     an adapter / write its own files.
 * They used to live in tui's `edc_c_bridge` (moved into app by 92b2c99), now their
 * own `//src/app:api` -- one rule: no business logic here, only projection.
 *
 * The only parameter is an opaque `struct edc_app *`: this file does not touch its
 * fields, reading only through the `edc_app_config()` / `edc_app_channel_actor()`
 * accessors (`app->` appears 0 times), so `struct edc_app` stays defined only in
 * `edc_app.c`.
 *
 * C++-safe (`edc_api.h` and `edc_app.h` share the rule: include no sds header). */
#include "edc_api.h"

#include "edc_actor.h"
#include "edc_app.h"
#include "edc_channel_actor.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "log.h"
#include "sds.h"
#include <stdio.h>
#include <string.h>

/* ── The frontend's read-only window (sds -> plain C types) ────
 * This file is C++-safe (it includes no sds header), so every sds field or
 * C-only detail is converted here. The C++ <-> C boundary belongs to app. */

_Static_assert(EDC_RENDER_MARKDOWN == EDC_APP_RENDER_MARKDOWN,
               "the EDC_APP_RENDER_* mirror in edc_api.h must match edc_render_pref_t");

const char *edc_app_message_content(const struct edc_channel_message *message)
{
    return message ? message->content : NULL;
}

const char *edc_app_message_header(const struct edc_channel_message *message)
{
    return message ? message->header : NULL;
}

/* The task id that produced this message; NULL when no task did (user input, a
 * snapshot). */
const char *edc_app_message_task_id(const struct edc_channel_message *message)
{
    return message ? message->task_id : NULL;
}

/* Which session the producing task belongs to; NULL when no task did. The
 * frontend uses it to record another session's background output into that
 * session's own transcript. */
const char *edc_app_message_session_id(const struct edc_channel_message *message)
{
    return message ? message->session_id : NULL;
}

int edc_app_message_is_user(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_USER_TYPE : 0;
}

int edc_app_message_is_tool(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_TOOL_TYPE : 0;
}

int edc_app_message_is_thought(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_THOUGHT_TYPE : 0;
}

int edc_app_message_is_resp(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_RESP_TYPE : 0;
}

int edc_app_message_is_sys(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_SYS_TYPE : 0;
}

/* The `/task history` reply payload (structured, not SYS text): the frontend
 * renders it as a message history. */
int edc_app_message_is_task_history(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_TASK_HISTORY_TYPE : 0;
}

/* A pull REPLY: the content is task-tree snapshot JSON to parse, never rendered as
 * chat text. */
int edc_app_message_is_task_snapshot(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE : 0;
}

/* An EVENT: "some session's tree changed" (id + reason only, no tree). */
int edc_app_message_is_task_tree_changed(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE : 0;
}

/* A fact the sender declares: whether this output came from its own session's
 * active task. The display gate lives in the frontend, which decides from the
 * session it is showing. */
int edc_app_message_from_active_task(const struct edc_channel_message *message)
{
    return (message && message->from_active_task) ? 1 : 0;
}

int edc_app_message_render_pref(const struct edc_channel_message *message)
{
    /* Returns EDC_APP_RENDER_PLAIN/MARKDOWN; PLAIN when the sender did not declare. */
    return message ? (int)message->render_pref : EDC_APP_RENDER_PLAIN;
}

/* ── Config / session string readers (sds -> caller buffer) ──── */

static bool copy_sds_into(const sds value, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    buf[0] = '\0';
    if (!value)
        return false;
    const bool ok = sdslen(value) < cap;
    if (ok)
        memcpy(buf, value, sdslen(value) + 1);
    return ok;
}

bool edc_app_config_model(const struct edc_app *app, char *buf, size_t cap)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg ? copy_sds_into(cfg->llm_api_conf.model, buf, cap) : copy_sds_into(NULL, buf, cap);
}

bool edc_app_config_api_url(const struct edc_app *app, char *buf, size_t cap)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg ? copy_sds_into(cfg->llm_api_conf.api_url, buf, cap) : copy_sds_into(NULL, buf, cap);
}

bool edc_app_config_api_key(const struct edc_app *app, char *buf, size_t cap)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg ? copy_sds_into(cfg->llm_api_conf.api_key, buf, cap) : copy_sds_into(NULL, buf, cap);
}

bool edc_app_sandbox_enabled(const struct edc_app *app)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg && cfg->enable_sandbox;
}

/* Where a transcript lives -- path resolution only; encode/append/replay are the
 * frontend's own concern (see edc_chat_transcript.hpp).
 *
 * The path is assembled by convention, not by querying the store: a session dir is
 * `<workspace>/<session_id>` (the same rule edc_session_new_with_workdir uses on
 * the kernel side; tests/test_session_workdir.c pins it), so the frontend never
 * touches the session store (mutated by the actor thread).
 *
 * |session_id| must be a session id (as carried by snapshots/messages). */
bool edc_app_chat_log_path_for(const struct edc_app *app, const char *session_id, char *buf, size_t cap)
{
    if (buf && cap > 0)
        buf[0] = '\0';
    struct edc_config *cfg = edc_app_config(app);
    if (!cfg || !cfg->workspace || sdslen(cfg->workspace) == 0 || !session_id || !*session_id || !buf || cap == 0)
        return false;

    sds path = edc_path_join(cfg->workspace, session_id, "chat.jsonl", NULL);
    const bool ok = path && strlen(path) < cap;
    if (ok)
        memcpy(buf, path, strlen(path) + 1);
    if (path)
        sdsfree(path);
    return ok;
}

/* The frontend's command history file (`<bc_home>/.tui_history`). */
bool edc_app_history_path(char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    buf[0] = '\0';

    sds home = edc_config_resolve_bc_home();
    if (!home)
        return false;
    sds path = edc_path_join(home, ".tui_history", NULL);
    sdsfree(home);
    if (!path)
        return false;

    const bool ok = sdslen(path) < cap;
    if (ok)
        memcpy(buf, path, sdslen(path) + 1);
    sdsfree(path);
    return ok;
}

/* ── A few things the frontend does ─────────────────────────── */

/* Register the frontend as the ONLY channel adapter: every on-screen message
 * reaches |on_send|. */
void edc_app_channel_adapter_init(struct edc_app *app, void *instance,
                                  void (*on_send)(void *, struct edc_channel_message *))
{
    edc_actor_t *channel = edc_app_channel_actor(app);
    if (!channel)
        return;
    edc_channel_adapter_init(channel, instance, on_send);
}

/* Send a user input. |session_id| names the session it belongs to (the task it
 * spawns lands there). NULL/"" = unspecified, the store picks the default. */
void edc_app_send_user_message(struct edc_app *app, const char *session_id, const char *content)
{
    edc_actor_t *channel = edc_app_channel_actor(app);
    if (!channel)
        return;

    struct edc_channel_message *message =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    if (session_id && *session_id)
        message->session_id = sdsnew(session_id);

    edc_channel_actor_state_t *state = channel->state;
    /* Boundary translation: core does not know channel messages (edc_channel_actor.h). */
    edc_actor_msg_t *actor_msg =
        edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, channel, NULL, edc_channel_input_new(message),
                          (edc_actor_data_free_fn)edc_channel_input_free);
    edc_actor_send(state->observer, actor_msg);
}

/* Request a task tree (the frontend pulls only the one it displays); built on the
 * scheduler thread and answered as TASK_SNAPSHOT through the channel adapter. */
void edc_app_request_task_snapshot(struct edc_app *app, const char *session_id)
{
    edc_actor_t *channel = edc_app_channel_actor(app);
    if (!channel)
        return;

    edc_channel_actor_state_t *state = channel->state;
    if (!state || !state->observer)
        return;

    sds tag = sdsnew(session_id ? session_id : "");
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, channel, NULL, tag,
                                             (edc_actor_data_free_fn)sdsfree);
    edc_actor_send(state->observer, msg);
}

bool edc_app_ensure_parent_dir(struct edc_app *app, const char *path)
{
    if (!path)
        return false;

    sds parent = edc_path_dirname(path);
    if (!parent || !parent[0])
    {
        if (parent)
            sdsfree(parent);
        return false;
    }

    /* Must stay under the workspace: everything written here is the frontend's own. */
    const struct edc_config *cfg = edc_app_config(app);
    const char *root = cfg ? cfg->workspace : NULL;
    bool ok = root && root[0] && strncmp(parent, root, strlen(root)) == 0;
    if (ok)
    {
        EdcError *err = NULL;
        ok = edc_dir_create_with_parents(parent, 0755, &err);
        if (err)
        {
            log_warn("edc_app_ensure_parent_dir: mkdir %s: %s", parent, err->message);
            edc_error_free(err);
        }
    }
    sdsfree(parent);
    return ok;
}

bool edc_app_write_file(struct edc_app *app, const char *path, const char *data, size_t len, bool append,
                        bool under_workspace)
{
    if (!path || (!data && len > 0))
        return false;

    const struct edc_config *cfg = edc_app_config(app);
    const char *root = (under_workspace && cfg) ? cfg->workspace : NULL;
    FILE *f = edc_file_fopen_safe(path, append ? "a" : "w", root);
    if (!f)
        return false;

    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    fclose(f);
    return ok;
}
