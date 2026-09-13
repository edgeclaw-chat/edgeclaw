#include "edc_c_bridge.h"
#include "edc_agent.h"
#include "edc_channel_actor.h"
#include "edc_commands.h"
#include "uv.h"
#include <cJSON.h>
#include <edc_actor.h>
#include <edc_file_utils.h>
#include <stdlib.h>
#include <string.h>

/*
 * Bridge implementation. Compiled as C (this is a .c file), so it can include
 * edc_agent.h and the sds/cJSON headers it pulls in. The TUI (C++) only sees
 * edc_c_bridge.h, which uses plain C types.
 */

struct edc_agent *edc_c_agent_new(bool setup, int sandbox_override)
{
    return edc_agent_new(setup, sandbox_override);
}

void edc_c_agent_set_command_handler(void)
{
    edc_agent_set_command_handler(edc_commands_handler);
}

void edc_c_agent_free(struct edc_agent *agent)
{
    edc_agent_free(agent);
}

struct edc_actor *edc_c_agent_channel_actor(struct edc_agent *agent)
{
    return agent ? agent->channel_actor : NULL;
}

const char *edc_c_channel_message_content(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->content : NULL;
}

const char *edc_c_channel_message_header(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->header : NULL;
}

/* Id of the task that produced this message (see struct edc_channel_message).
 * NULL when the sender is not a task (user input, snapshot, ...). The TUI uses
 * it to label chat messages with their task ("task 1"). */
const char *edc_c_channel_message_task_id(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->task_id : NULL;
}

/* Id of the session that owns the producing task (see struct edc_channel_message).
 * NULL when the sender is not a task. The TUI uses it to route a background
 * session's output into that session's transcript instead of the displayed one. */
const char *edc_c_channel_message_session_id(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->session_id : NULL;
}

int edc_c_channel_message_is_user(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_USER_TYPE : 0;
}

int edc_c_channel_message_is_tool(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_TOOL_TYPE : 0;
}

int edc_c_channel_message_is_thought(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_THOUGHT_TYPE : 0;
}

int edc_c_channel_message_is_resp(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_RESP_TYPE : 0;
}

int edc_c_channel_message_is_sys(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_SYS_TYPE : 0;
}

int edc_c_channel_message_is_task_snapshot(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->message_type == EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE : 0;
}

int edc_c_channel_message_render_pref(struct edc_channel_message *channel_message)
{
    /* 返回 EDC_C_RENDER_PLAIN/MARKDOWN；发送方未声明时为 PLAIN（零值默认） */
    return channel_message ? (int)channel_message->render_pref : EDC_C_RENDER_PLAIN;
}

void edc_c_agent_request_task_snapshot(struct edc_agent *agent)
{
    if (!agent || !agent->channel_actor)
        return;

    edc_channel_actor_state_t *state = agent->channel_actor->state;
    if (!state || !state->observer)
        return;

    /* observer = scheduler actor; the snapshot is built on its thread. */
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, agent->channel_actor, NULL, NULL, NULL);
    edc_actor_send(state->observer, msg);
}

/* ── Task-tree snapshot JSON parsing ───────────────────────── */

static char *snap_dup(const char *s)
{
    return s ? strdup(s) : strdup("");
}

static const char *snap_str(const cJSON *item)
{
    return cJSON_IsString(item) ? item->valuestring : "";
}

/* Read a 64-bit integer from a snapshot node; 0 when absent or not a number.
 * cJSON has no integer type, so counts arrive as doubles and are narrowed
 * back here (token totals stay far below 2^53). */
static long long snap_ll(const cJSON *item)
{
    return cJSON_IsNumber(item) ? (long long)item->valuedouble : 0;
}

/* forward: per-task todo items live in the snapshot tree (defined below) */
static struct edc_c_todo_item *snap_todo_parse(const cJSON *arr, int *count_out);
static void snap_todo_free(struct edc_c_todo_item *items, int count);

struct edc_c_task_snapshot *edc_c_task_snapshot_parse(const char *json)
{
    if (!json)
        return NULL;

    cJSON *root = cJSON_Parse(json);
    if (!root)
        return NULL;

    struct edc_c_task_snapshot *snap = calloc(1, sizeof(*snap));
    if (!snap)
    {
        cJSON_Delete(root);
        return NULL;
    }

    snap->session_id = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(root, "session_id")));
    snap->session_name = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(root, "session_name")));
    cJSON *mc = cJSON_GetObjectItemCaseSensitive(root, "message_count");
    snap->message_count = cJSON_IsNumber(mc) ? mc->valueint : 0;
    snap->created_at = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(root, "created_at")));

    cJSON *groups = cJSON_GetObjectItemCaseSensitive(root, "groups");
    if (cJSON_IsArray(groups))
    {
        snap->group_count = cJSON_GetArraySize(groups);
        snap->groups = calloc((size_t)snap->group_count, sizeof(struct edc_c_task_group));

        int gi = 0;
        cJSON *gj;
        cJSON_ArrayForEach(gj, groups)
        {
            struct edc_c_task_group *g = &snap->groups[gi++];
            cJSON *mc = cJSON_GetObjectItemCaseSensitive(gj, "member_count");
            cJSON *gmc = cJSON_GetObjectItemCaseSensitive(gj, "msg_count");
            g->group_id = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(gj, "id")));
            g->member_count = cJSON_IsNumber(mc) ? mc->valueint : 0;
            g->msg_count = cJSON_IsNumber(gmc) ? gmc->valueint : 0;
            g->orphaned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(gj, "orphaned")) ? 1 : 0;
            g->fg = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(gj, "fg")) ? 1 : 0;
            g->tokens_total = snap_ll(cJSON_GetObjectItemCaseSensitive(gj, "tokens_total"));

            cJSON *tasks = cJSON_GetObjectItemCaseSensitive(gj, "tasks");
            if (cJSON_IsArray(tasks))
            {
                g->task_count = cJSON_GetArraySize(tasks);
                g->tasks = calloc((size_t)g->task_count, sizeof(struct edc_c_task_info));

                int ti = 0;
                cJSON *tj;
                cJSON_ArrayForEach(tj, tasks)
                {
                    struct edc_c_task_info *t = &g->tasks[ti++];
                    t->task_id = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(tj, "id")));
                    t->state = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(tj, "state")));
                    t->is_leader = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tj, "leader")) ? 1 : 0;
                    t->is_active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tj, "active")) ? 1 : 0;
                    t->description = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(tj, "desc")));
                    t->tokens_prompt = snap_ll(cJSON_GetObjectItemCaseSensitive(tj, "tokens_prompt"));
                    t->tokens_completion = snap_ll(cJSON_GetObjectItemCaseSensitive(tj, "tokens_completion"));
                    t->tokens_total = snap_ll(cJSON_GetObjectItemCaseSensitive(tj, "tokens_total"));
                    t->tokens_cached = snap_ll(cJSON_GetObjectItemCaseSensitive(tj, "tokens_cached"));
                    cJSON *todo = cJSON_GetObjectItemCaseSensitive(tj, "todos");
                    if (cJSON_IsArray(todo))
                        t->todo_items = snap_todo_parse(todo, &t->todo_count);
                }
            }
        }
    }

    cJSON_Delete(root);
    return snap;
}

void edc_c_task_snapshot_free(struct edc_c_task_snapshot *snap)
{
    if (!snap)
        return;

    free((void *)snap->session_id);
    free((void *)snap->session_name);
    free((void *)snap->created_at);
    for (int gi = 0; gi < snap->group_count; gi++)
    {
        struct edc_c_task_group *g = &snap->groups[gi];
        free((void *)g->group_id);
        for (int ti = 0; ti < g->task_count; ti++)
        {
            struct edc_c_task_info *t = &g->tasks[ti];
            free((void *)t->task_id);
            free((void *)t->state);
            free((void *)t->description);
            snap_todo_free(t->todo_items, t->todo_count);
        }
        free(g->tasks);
    }
    free(snap->groups);
    free(snap);
}

/* ── Todo item array parsing (per-task "todos" in a snapshot) ─ */

/* Parse a JSON items array (todowrite items: each with content/status/
 * priority) into a malloc'd array of edc_c_todo_item. Returns NULL and sets
 * *count_out = 0 when arr is not an array or has no elements. The caller
 * frees the result with snap_todo_free(). */
static struct edc_c_todo_item *snap_todo_parse(const cJSON *arr, int *count_out)
{
    *count_out = 0;
    if (!cJSON_IsArray(arr))
        return NULL;

    const int n = cJSON_GetArraySize(arr);
    if (n <= 0)
        return NULL;

    struct edc_c_todo_item *items = calloc((size_t)n, sizeof(*items));
    if (!items)
        return NULL;

    int i = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        struct edc_c_todo_item *dst = &items[i++];
        dst->content = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(it, "content")));
        dst->status = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(it, "status")));
        dst->priority = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(it, "priority")));
    }
    *count_out = n;
    return items;
}

static void snap_todo_free(struct edc_c_todo_item *items, int count)
{
    if (!items)
        return;
    for (int i = 0; i < count; i++)
    {
        free((void *)items[i].content);
        free((void *)items[i].status);
        free((void *)items[i].priority);
    }
    free(items);
}

void edc_c_file_utils_init(struct uv_loop_s *loop)
{
    edc_file_utils_init(loop);
}

bool edc_c_agent_model(struct edc_agent *agent, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    buf[0] = '\0';
    if (!agent || !agent->config)
        return false;

    const sds model = agent->config->llm_api_conf.model;
    if (!model)
        return false;

    const bool ok = sdslen(model) < cap;
    if (ok)
        memcpy(buf, model, sdslen(model) + 1);
    return ok;
}

static bool edc_c_copy_sds(const sds value, char *buf, size_t cap)
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

bool edc_c_agent_sandbox_enabled(struct edc_agent *agent)
{
    return agent && agent->config && agent->config->enable_sandbox;
}

bool edc_c_agent_api_url(struct edc_agent *agent, char *buf, size_t cap)
{
    if (!agent || !agent->config)
    {
        if (buf && cap)
            buf[0] = '\0';
        return false;
    }
    return edc_c_copy_sds(agent->config->llm_api_conf.api_url, buf, cap);
}

bool edc_c_agent_api_key(struct edc_agent *agent, char *buf, size_t cap)
{
    if (!agent || !agent->config)
    {
        if (buf && cap)
            buf[0] = '\0';
        return false;
    }
    return edc_c_copy_sds(agent->config->llm_api_conf.api_key, buf, cap);
}

bool edc_c_agent_connect(struct edc_agent *agent, const char *api_url, const char *model, const char *api_key)
{
    return edc_agent_set_llm_config(agent, api_url, model, api_key);
}

bool edc_c_history_path(char *buf, size_t cap)
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

bool edc_c_agent_session_id(struct edc_agent *agent, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    buf[0] = '\0';

    if (!agent || !agent->session_manager)
        return false;
    struct edc_session *session = edc_session_manager_get_active(agent->session_manager);
    if (!session || !session->session_id)
        return false;

    const size_t n = strlen(session->session_id);
    if (n >= cap)
        return false;
    memcpy(buf, session->session_id, n + 1);
    return true;
}

bool edc_c_session_workdir(struct edc_agent *agent, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    buf[0] = '\0';

    if (!agent || !agent->session_manager)
        return false;
    struct edc_session *session = edc_session_manager_get_active(agent->session_manager);
    const char *workdir = session ? edc_session_get_workdir(session) : NULL;
    if (!workdir || !workdir[0])
        return false;

    const size_t n = strlen(workdir);
    if (n >= cap)
        return false;
    memcpy(buf, workdir, n + 1);
    return true;
}

static bool chat_log_path_for_session(struct edc_session *session, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    buf[0] = '\0';
    if (!session || !session->history_dir)
        return false;

    /* The transcript lives beside the session's state; make sure the session
     * directory exists so the TUI can append to it before the first state
     * save has run (a fresh session has no directory yet). Best-effort. */
    EdcError *err = NULL;
    edc_dir_create_with_parents(session->history_dir, 0755, &err);
    if (err)
        edc_error_free(err);

    sds path = edc_path_join(session->history_dir, "chat.jsonl", NULL);
    if (!path)
        return false;

    const bool ok = sdslen(path) < cap;
    if (ok)
        memcpy(buf, path, sdslen(path) + 1);
    sdsfree(path);
    return ok;
}

bool edc_c_session_chat_log_path(struct edc_agent *agent, char *buf, size_t cap)
{
    if (!agent || !agent->session_manager)
    {
        if (buf && cap > 0)
            buf[0] = '\0';
        return false;
    }
    return chat_log_path_for_session(edc_session_manager_get_active(agent->session_manager), buf, cap);
}

/* Like edc_c_session_chat_log_path, but for an arbitrary session id — lets the
 * TUI append a background session's output to *its own* transcript even while a
 * different session is displayed. */
bool edc_c_session_chat_log_path_for(struct edc_agent *agent, const char *session_id, char *buf, size_t cap)
{
    if (!agent || !agent->session_manager || !session_id)
    {
        if (buf && cap > 0)
            buf[0] = '\0';
        return false;
    }
    return chat_log_path_for_session(edc_session_manager_find(agent->session_manager, session_id), buf, cap);
}

bool edc_c_agent_write_file(struct edc_agent *agent, const char *path, const char *data, size_t len, bool append,
                            bool under_workspace)
{
    if (!path || (!data && len > 0))
        return false;

    const char *root = (under_workspace && agent && agent->config) ? agent->config->workspace : NULL;
    FILE *f = edc_file_fopen_safe(path, append ? "a" : "w", root);
    if (!f)
        return false;

    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    fclose(f);
    return ok;
}

void edc_c_agent_send_user_message(struct edc_agent *agent, const char *content)
{
    if (!agent)
        return;

    struct edc_channel_message *message =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));

    edc_channel_actor_state_t *state = agent->channel_actor->state;
    edc_actor_msg_t *actor_msg = edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, agent->channel_actor, NULL, message,
                                                   (edc_actor_data_free_fn)edc_channel_message_free);
    edc_actor_send(state->observer, actor_msg);
}
