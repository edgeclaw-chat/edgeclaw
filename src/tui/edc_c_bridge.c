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

struct edc_agent *edc_c_agent_new(bool setup, bool enable_sandbox)
{
    return edc_agent_new(setup, enable_sandbox);
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

const char *edc_c_channel_message_payload(struct edc_channel_message *channel_message)
{
    return channel_message ? channel_message->payload : NULL;
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
    edc_actor_msg_t *msg =
        edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, agent->channel_actor, NULL, NULL, NULL);
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
            g->group_id = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(gj, "id")));
            g->member_count = cJSON_IsNumber(mc) ? mc->valueint : 0;
            g->orphaned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(gj, "orphaned")) ? 1 : 0;
            g->fg = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(gj, "fg")) ? 1 : 0;

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
        }
        free(g->tasks);
    }
    free(snap->groups);
    free(snap);
}

/* ── Todo payload JSON parsing ─────────────────────────────── */

/* Parse the structured payload of an update_todolist tool message — the raw
 * tool arguments {"items":[{content,status,activeForm},...]}. Accepts either
 * that object form or a bare items array. Returns NULL on parse failure.
 * Free with edc_c_todo_payload_free(). */
struct edc_c_todo_payload *edc_c_todo_payload_parse(const char *json)
{
    if (!json)
        return NULL;

    cJSON *root = cJSON_Parse(json);
    if (!root)
        return NULL;

    cJSON *items = root;
    if (cJSON_IsObject(root))
    {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "items");
        if (!cJSON_IsArray(arr))
        {
            cJSON_Delete(root);
            return NULL;
        }
        items = arr;
    }
    if (!cJSON_IsArray(items))
    {
        cJSON_Delete(root);
        return NULL;
    }

    struct edc_c_todo_payload *out = calloc(1, sizeof(*out));
    out->item_count = cJSON_GetArraySize(items);
    if (out->item_count > 0)
        out->items = calloc((size_t)out->item_count, sizeof(struct edc_c_todo_item));

    int i = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        struct edc_c_todo_item *dst = &out->items[i++];
        dst->content = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(it, "content")));
        dst->status = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(it, "status")));
        dst->active_form = snap_dup(snap_str(cJSON_GetObjectItemCaseSensitive(it, "activeForm")));
    }

    cJSON_Delete(root);
    return out;
}

void edc_c_todo_payload_free(struct edc_c_todo_payload *payload)
{
    if (!payload)
        return;
    for (int i = 0; i < payload->item_count; i++)
    {
        free((void *)payload->items[i].content);
        free((void *)payload->items[i].status);
        free((void *)payload->items[i].active_form);
    }
    free(payload->items);
    free(payload);
}

void edc_c_file_utils_init(struct uv_loop_s *loop)
{
    edc_file_utils_init(loop);
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
