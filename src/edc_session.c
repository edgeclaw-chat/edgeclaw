#include "sds.h"
#include "edc_session.h"
#include <glib.h>
#include "edc_config.h"
#include "edc_context.h"
#include "edc_history.h"
#include "edc_turn.h"
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
    char *valid_utf8 = g_utf8_make_valid(system_prompt, -1);
    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", valid_utf8);
    free(valid_utf8);
    sdsfree(system_prompt);

    cJSON_AddItemToArray(session->messages, system_msg);

    if (load_history)
    {
        size_t compress_index = 0;
        edc_history_get_info(session->config->workspace, &compress_index, NULL);
        cJSON *history = edc_history_load(session->config->workspace, compress_index);
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

struct edc_session *edc_session_new(struct edc_config *config)
{
    struct edc_session *session = g_new0(struct edc_session, 1);
    session->config = config;
    session->messages = cJSON_CreateArray();
    edc_session_build_messages(session, NULL, TRUE);
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
    bool ok = edc_history_save(session->config->workspace, session->messages, session->timestamps,
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
