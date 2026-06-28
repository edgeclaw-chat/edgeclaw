#include "edc_turn.h"
#include "edc_history.h"
#include <glib.h>

static void edc_turn_add_message(struct edc_turn *turn, cJSON *msg)
{
    cJSON *content = cJSON_GetObjectItem(msg, "content");
    char *valid_utf8 = g_utf8_make_valid(cJSON_GetStringValue(content), -1);
    cJSON *valid_content = cJSON_CreateString(valid_utf8);
    free(valid_utf8);
    cJSON_ReplaceItemInObject(msg, "content", valid_content);

    edc_queue_push(&turn->messages, msg);
    edc_queue_push(&turn->timestamps, edc_get_timestamp());
}

void edc_turn_add_user_message(struct edc_turn *turn, const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", content);
    edc_turn_add_message(turn, msg);
}

void edc_turn_add_assistant_message(struct edc_turn *turn, cJSON *msg)
{
    edc_turn_add_message(turn, msg);
}

void edc_turn_add_tool_message(struct edc_turn *turn, const char *tool_call_id, const char *name, const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", tool_call_id);
    cJSON_AddStringToObject(msg, "name", name);
    cJSON_AddStringToObject(msg, "content", content);
    edc_turn_add_message(turn, msg);
}

struct edc_turn *edc_turn_new()
{
    struct edc_turn *turn = g_new0(struct edc_turn, 1);
    edc_queue_init(&turn->messages);
    edc_queue_init(&turn->timestamps);
    return turn;
}

void edc_turn_free(struct edc_turn *turn)
{
    if (!turn)
    {
        return;
    }

    edc_queue_destroy(&turn->messages);
    edc_queue_destroy(&turn->timestamps);

    free(turn);
    return;
}

