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
#include "edc_sandbox.h"
#include "edc_file_index.h"
#include "edc_path_guard.h"

#include "edc_actor.h"
#include "edc_app.h"
#include "edc_channel_actor.h"
#include "edc_cmd_parse.h"
#include "edc_commands.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_models.h"
#include "edc_prompts.h"
#include "edc_resources.h"
#include "edc_skills.h"
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

bool edc_app_shell_syntax(const struct edc_app *app)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg && cfg->shell_syntax;
}

edc_input_mode edc_app_input_mode(const struct edc_app *app, const char *text)
{
    struct edc_config *cfg = edc_app_config(app);
    sds err = NULL;
    const edc_in_kind kind = edc_commands_input_kind(cfg, text, &err);
    if (err)
    {
        sdsfree(err);
        return EDC_INPUT_REJECTED;
    }
    switch (kind)
    {
    case EDC_IN_COMMAND:
        return EDC_INPUT_COMMAND;
    case EDC_IN_LOCALSHELL:
        return EDC_INPUT_LOCALSHELL;
    case EDC_IN_SHELL:
        return EDC_INPUT_SHELL;
    case EDC_IN_MESSAGE:
    default:
        return EDC_INPUT_TEXT;
    }
}

int edc_app_onboarded(const struct edc_app *app)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg && cfg->onboarded;
}

int edc_app_config_context_window(const struct edc_app *app)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg ? cfg->context_window : 0;
}

/* ── Model catalog: pure projection of edc_models (see edc_api.h) ── */

size_t edc_app_models_providers(void)
{
    return edc_models_provider_count();
}
const char *edc_app_models_provider_id(size_t index)
{
    return edc_models_provider_id(index);
}
const char *edc_app_models_provider_name(const char *provider_id)
{
    return edc_models_provider_name(provider_id);
}
const char *edc_app_models_provider_api(const char *provider_id)
{
    return edc_models_provider_api(provider_id);
}
const char *edc_app_models_provider_env_name(const char *provider_id, size_t index)
{
    return edc_models_provider_env_name(provider_id, index);
}
size_t edc_app_models_count(const char *provider_id)
{
    return edc_models_model_count(provider_id);
}
const char *edc_app_models_model_id(const char *provider_id, size_t index)
{
    return edc_models_model_id_at(provider_id, index);
}
const char *edc_app_models_model_name(const char *model_id)
{
    struct edc_model_info info;
    return edc_models_model_info(model_id, &info) ? info.name : NULL;
}
int edc_app_models_model_context(const char *model_id)
{
    struct edc_model_info info;
    return edc_models_model_info(model_id, &info) ? (int)info.context : 0;
}
int edc_app_models_model_output(const char *model_id)
{
    struct edc_model_info info;
    return edc_models_model_info(model_id, &info) ? (int)info.output : 0;
}
int edc_app_models_model_is_chat(const char *model_id)
{
    return edc_models_is_chat_model(model_id);
}
int edc_app_models_provider_matches(const char *provider_id, const char *query)
{
    return edc_models_provider_matches(provider_id, query);
}
int edc_app_models_model_matches(const char *model_id, const char *query)
{
    return edc_models_model_matches(model_id, query);
}

/* The command surface, straight from the shell's table (static data, no app). */
size_t edc_app_cmd_count(void)
{
    return edc_commands_doc_count();
}
const char *edc_app_cmd_text(size_t index)
{
    return edc_commands_doc_cmd(index);
}
const char *edc_app_cmd_brief(size_t index)
{
    return edc_commands_doc_brief(index);
}
size_t edc_app_cmd_family_count(void)
{
    return edc_commands_family_count();
}
const char *edc_app_cmd_family_name(size_t index)
{
    return edc_commands_family_name(index);
}
const char *edc_app_cmd_family_brief(size_t index)
{
    return edc_commands_family_brief(index);
}

const char *edc_app_shell_option_text(size_t index)
{
    return edc_cmd_shell_option_text(index);
}
size_t edc_app_shell_option_count(void)
{
    return edc_cmd_shell_option_count();
}
const char *edc_app_shell_option_brief(size_t index)
{
    return edc_cmd_shell_option_brief(index);
}

/* The prompt templates under <workspace>/prompts/, as "<name>\t<description>" lines.
 * The frontend's completion popup offers `/prompt <name>`, so it needs the names. */
size_t edc_app_prompts(const struct edc_app *app, const char *workdir, char *buf, size_t cap)
{
    if (buf && cap > 0)
        buf[0] = '\0';
    const struct edc_config *cfg = edc_app_config(app);
    if (!cfg || !cfg->workspace)
        return 0;

    /* Every resource root, ours first -- so a template migrated from pi / Claude Code /
     * opencode is offered by the completion popup without being moved. The project roots
     * come from the displayed session's directory. */
    UT_array *roots = edc_resource_dirs(cfg, workdir);
    UT_array *prompts = edc_prompts_list_roots(roots, cfg);
    edc_resource_dirs_free(roots);
    const size_t count = utarray_len(prompts);

    size_t used = 0;
    for (size_t i = 0; i < count; i++)
    {
        struct edc_prompt **slot = (struct edc_prompt **)utarray_eltptr(prompts, (unsigned)i);
        struct edc_prompt *p = slot ? *slot : NULL;
        if (p && p->name)
        {
            sds line = sdscatprintf(sdsempty(), "%s\t%s\n", p->name,
                                    (p->description && sdslen(p->description)) ? p->description : "");
            if (buf && used + sdslen(line) + 1 <= cap)
            {
                memcpy(buf + used, line, sdslen(line));
                used += sdslen(line);
                buf[used] = '\0';
            }
            sdsfree(line);
        }
        edc_prompt_free(p);
    }
    utarray_free(prompts);
    return count;
}

size_t edc_app_file_candidates(struct edc_app *app, const char *workdir, const char *query, char *buf, size_t cap)
{
    if (buf && cap > 0)
        buf[0] = '\0';
    const struct edc_config *cfg = edc_app_config(app);
    if (!cfg)
        return 0;

    /* Which directory to list: the SESSION's bound directory, and nothing else. The list is
     * what the user is working in -- it is not a permission check, so the rw/ro lists are
     * not applied (they say what the model may touch, and a session is allowed to be bound
     * to a repository outside them; the tools refuse individual reads at their own layer).
     * Applying them here made the picker come up empty for exactly those sessions. The deny
     * list still applies, the same rule the prompt/skill readers use.
     *
     * An unbound session falls back to the LAUNCH directory (edc_cwd()), which is what
     * every other "where am I" answer in the app uses (edc_resource_dirs walks up from
     * there). Falling back to cfg->workspace was wrong: that is the agent's own data
     * directory, not the project the person is looking at. */
    sds fallback = NULL;
    const char *root = (workdir && workdir[0]) ? workdir : NULL;
    if (!root)
    {
        fallback = edc_cwd();
        root = fallback;
    }

    struct edc_path_rules rules = {
        .root = NULL, /* a listing root: no containment boundary above it */
        .refuse_symlink = false,
        .deny_paths = cfg->enable_sandbox ? cfg->sandbox_deny_paths : NULL,
    };
    EdcError *error = NULL;
    sds safe = edc_path_guard_resolve(&rules, root, &error);
    if (!safe)
    {
        /* A directory that cannot be resolved is worth one line in the log: otherwise the
         * popup simply never opens and nothing says why. */
        log_warn("file candidates: cannot list '%s': %s", root, error && error->message ? error->message : "?");
        if (error)
            edc_error_free(error);
        if (fallback)
            sdsfree(fallback);
        return 0;
    }
    const size_t rows = edc_file_index_query(safe, query, buf, cap);
    if (rows == 0)
        log_info("file candidates: none for root='%s' query='%s'", safe, query ? query : "");
    sdsfree(safe);
    if (fallback)
        sdsfree(fallback);
    return rows;
}

void edc_app_file_candidates_reset(void)
{
    edc_file_index_reset();
}

size_t edc_app_skills(const struct edc_app *app, const char *workdir, char *buf, size_t cap)
{
    if (buf && cap > 0)
        buf[0] = '\0';
    const struct edc_config *cfg = edc_app_config(app);
    if (!cfg || !cfg->workspace)
        return 0;

    UT_array *roots = edc_resource_dirs(cfg, workdir);
    UT_array *skills = edc_skills_load_roots(roots, cfg);
    edc_resource_dirs_free(roots);
    const size_t count = utarray_len(skills);

    size_t used = 0;
    for (size_t i = 0; i < count; i++)
    {
        struct edc_skill **slot = (struct edc_skill **)utarray_eltptr(skills, (unsigned)i);
        struct edc_skill *sk = slot ? *slot : NULL;
        if (sk && sk->name)
        {
            struct edc_skill_meta_entry *e = NULL;
            HASH_FIND_STR(sk->meta, "description", e);
            sds line =
                sdscatprintf(sdsempty(), "%s\t%s\n", sk->name, (e && e->value && sdslen(e->value)) ? e->value : "");
            if (buf && used + sdslen(line) + 1 <= cap)
            {
                memcpy(buf + used, line, sdslen(line));
                used += sdslen(line);
                buf[used] = '\0';
            }
            sdsfree(line);
        }
        edc_skill_free(sk);
    }
    utarray_free(skills);
    return count;
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
