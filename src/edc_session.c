#include "sds.h"
#include "edc_session.h"
#include "edc_compat.h"
#include "edc_string_utils.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_file_utils.h"
#include "edc_history.h"
#include "edc_datetime_utils.h"
#include "edc_turn.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include <string.h>

static void edc_session_trim_orphan_tool_messages(cJSON *history)
{
    while (cJSON_GetArraySize(history) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(history, 0);
        cJSON *role = cJSON_GetObjectItem(first, "role");
        if (role && cJSON_IsString(role) && strcmp(cJSON_GetStringValue(role), "tool") == 0)
        {
            cJSON_DeleteItemFromArray(history, 0);
        }
        else
        {
            break;
        }
    }
}

static void edc_session_merge_history(struct edc_session *session, cJSON *history)
{
    while (cJSON_GetArraySize(history) > 0)
    {
        cJSON *msg = cJSON_DetachItemFromArray(history, 0);
        if (cJSON_HasObjectItem(msg, "timestamp"))
        {
            cJSON_DeleteItemFromObject(msg, "timestamp");
        }
        cJSON_AddItemToArray(session->messages, msg);
    }
}

static void edc_session_build_messages(struct edc_session *session, const char *user_input, bool load_history)
{
    sds system_prompt = edc_context_build_system_prompt(session->config);

    cJSON *system_msg = cJSON_CreateObject();
    sds valid_utf8 = edc_str_utf8_make_valid(system_prompt, -1);
    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", valid_utf8);
    sdsfree(valid_utf8);
    sdsfree(system_prompt);

    cJSON_AddItemToArray(session->messages, system_msg);

    if (load_history)
    {
        size_t compress_index = 0;
        edc_history_get_info(session->history_dir, &compress_index, NULL);
        cJSON *history = edc_history_load(session->history_dir, compress_index);
        if (history != NULL)
        {
            edc_session_trim_orphan_tool_messages(history);
            edc_session_merge_history(session, history);
            cJSON_Delete(history);
        }
    }

    if (user_input)
    {
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", user_input);
        cJSON_AddItemToArray(session->messages, user_msg);
    }
}

struct edc_session *edc_session_new(struct edc_config *config,
                                    const char *session_id,
                                    const char *name)
{
    struct edc_session *session = g_new0(struct edc_session, 1);
    session->session_id = strdup(session_id ? session_id : "sess-1");
    session->name = sdsnew(name && strlen(name) > 0 ? name : session->session_id);
    session->created_at = edc_datetime_timestamp();
    session->last_active = edc_datetime_timestamp();
    session->config = config;
    session->messages = cJSON_CreateArray();
    session->history_dir = edc_path_join(config->workspace, session->session_id, NULL);
    session->next_job_id = 1;
    session->next_completion_seq = 1;
    edc_session_build_messages(session, NULL, true);
    session->save_skip_count = edc_session_get_message_count(session);
    return session;
}

void edc_session_reload(struct edc_session *session, bool load_history)
{
    edc_session_save_history(session);

    while (cJSON_GetArraySize(session->messages) > 0)
    {
        cJSON_DeleteItemFromArray(session->messages, 0);
    }

    edc_session_build_messages(session, NULL, load_history);

    session->save_skip_count = edc_session_get_message_count(session);
}

void edc_session_free(struct edc_session *session)
{
    if (!session)
        return;

    if (session->messages)
    {
        cJSON_Delete(session->messages);
    }

    struct edc_timestamp_node *n, *tmp;
    DL_FOREACH_SAFE(session->timestamps, n, tmp)
    {
        DL_DELETE(session->timestamps, n);
        sdsfree(n->ts);
        free(n);
    }

    /* groups freed by edc_scheduler_free (shared list) */

    free(session->session_id);
    sdsfree(session->name);
    sdsfree(session->created_at);
    sdsfree(session->last_active);
    sdsfree(session->history_dir);
    free(session);
}

void edc_session_add_turn(struct edc_session *sess, struct edc_turn *turn)
{
    EdcQueue *m = &turn->messages;
    EdcQueue *t = &turn->timestamps;
    while (edc_queue_length(m) > 0 && edc_queue_length(t) > 0)
    {
        cJSON *message = edc_queue_pop(m);
        sds ts = edc_queue_pop(t);
        cJSON_AddItemToArray(sess->messages, message);
        struct edc_timestamp_node *n = g_new0(struct edc_timestamp_node, 1);
        n->ts = ts;
        DL_APPEND(sess->timestamps, n);
    }
}

cJSON *edc_session_get_messages(struct edc_session *session)
{
    return session->messages;
}

size_t edc_session_get_message_count(struct edc_session *session)
{
    return cJSON_GetArraySize(session->messages);
}

bool edc_session_save_history(struct edc_session *session)
{
    bool ok = edc_history_save(session->history_dir, session->messages, session->timestamps,
                                  session->save_skip_count);
    if (ok)
    {
        session->save_skip_count = edc_session_get_message_count(session);
        struct edc_timestamp_node *n, *tmp;
        DL_FOREACH_SAFE(session->timestamps, n, tmp)
        {
            DL_DELETE(session->timestamps, n);
            sdsfree(n->ts);
            free(n);
        }
        session->timestamps = NULL;
    }

    return ok;
}

/* ══════════════════════════════════════════════════════════════
   Phase 4: Task/Group 管理
   ══════════════════════════════════════════════════════════════ */

void edc_session_add_group(struct edc_session *session, struct edc_task_group *group)
{
    if (!session || !group) return;
    DL_APPEND(session->groups, group);
}

void edc_session_remove_group(struct edc_session *session, struct edc_task_group *group)
{
    if (!session || !group) return;
    DL_DELETE(session->groups, group);
}

struct edc_task_group *edc_session_find_group(struct edc_session *session, const char *group_id)
{
    if (!session || !group_id) return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        if (strcmp(group->group_id, group_id) == 0) return group;
    }
    return NULL;
}

struct edc_task *edc_session_find_task(struct edc_session *session, const char *task_id)
{
    if (!session || !task_id) return NULL;
    struct edc_task_group *group;
    DL_FOREACH(session->groups, group)
    {
        struct edc_task *t = edc_task_group_find_task(group, task_id);
        if (t) return t;
    }
    return NULL;
}

void edc_session_set_active_task(struct edc_session *session, struct edc_task *task)
{
    if (!session) return;
    session->active_task = task;
}

struct edc_task *edc_session_get_active_task(struct edc_session *session)
{
    return session ? session->active_task : NULL;
}

/* ── 消息切片 ──────────────────────────────────────────────── */

cJSON *edc_session_get_messages_slice(struct edc_session *session, size_t from, size_t count)
{
    cJSON *slice = cJSON_CreateArray();
    if (!session || !session->messages) return slice;

    size_t total = cJSON_GetArraySize(session->messages);
    if (from >= total) return slice;

    size_t end = from + count;
    if (count == 0 || end > total) end = total;

    for (size_t i = from; i < end; i++)
    {
        cJSON *msg = cJSON_GetArrayItem(session->messages, i);
        cJSON_AddItemReferenceToArray(slice, msg);
    }
    return slice;
}

/* ── Task 结构持久化 ──────────────────────────────────────── */

bool edc_session_save_tasks(struct edc_session *session)
{
    if (!session) return false;

    /* 给 messages 中每条 non-system non-tool 消息追加 task_id */
    /* system 消息和 tool 消息不标记 (属于所有 task 共享) */

    cJSON *messages = session->messages;
    size_t count = cJSON_GetArraySize(messages);

    for (size_t i = 0; i < count; i++)
    {
        cJSON *msg = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        if (!role || !cJSON_IsString(role)) continue;

        const char *role_s = cJSON_GetStringValue(role);
        if (strcmp(role_s, "system") == 0 || strcmp(role_s, "tool") == 0) continue;

        if (!cJSON_HasObjectItem(msg, "task_id"))
        {
            /* 使用默认 "legacy" task_id */
            cJSON_AddStringToObject(msg, "task_id", "legacy");
            cJSON_AddStringToObject(msg, "group_id", "legacy");
        }
    }

    return edc_session_save_history(session);
}

void edc_session_reload_tasks(struct edc_session *session)
{
    if (!session || !session->messages) return;

    /* 从 messages 中提取所有 unique task_id，为每个创建一个 task */
    /* 简化为: 扫描 messages, 收集唯一 task_id */
    /* 完整实现留 Phase 5 在 edc_agent 中处理 */
    (void)session;
}

/* ── Multi-session support ─────────────────────────────────── */

void edc_session_reset(struct edc_session *session)
{
    if (!session) return;

    /* Clear messages */
    while (cJSON_GetArraySize(session->messages) > 0)
        cJSON_DeleteItemFromArray(session->messages, 0);

    /* Clear timestamps */
    struct edc_timestamp_node *n, *tmp;
    DL_FOREACH_SAFE(session->timestamps, n, tmp)
    {
        DL_DELETE(session->timestamps, n);
        sdsfree(n->ts);
        free(n);
    }
    session->timestamps = NULL;

    /* Reset save skip count */
    session->save_skip_count = 0;

    /* Clear active task */
    session->active_task = NULL;

    /* groups are owned by scheduler — just clear the list */
    session->groups = NULL;
}

const char *edc_session_get_name(struct edc_session *session)
{
    if (!session) return NULL;
    return session->name ? session->name : session->session_id;
}

