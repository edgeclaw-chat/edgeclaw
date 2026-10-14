#include "edc_turn.h"
#include "edc_datetime_utils.h"
#include "edc_compat.h"
#include "edc_string_utils.h"

void edc_turn_add_message(struct edc_turn *turn, cJSON *msg)
{
    cJSON *content = cJSON_GetObjectItem(msg, "content");
    sds valid_utf8 = edc_str_utf8_make_valid(cJSON_GetStringValue(content), -1);
    cJSON *valid_content = cJSON_CreateString(valid_utf8);
    sdsfree(valid_utf8);
    cJSON_ReplaceItemInObject(msg, "content", valid_content);

    /* v3: a message carries its own timestamp and render preference, persisted with
     * it -- no parallel timestamps queue (which was lost on restart and needed
     * indices aligned with messages). */
    if (!cJSON_HasObjectItem(msg, "ts"))
    {
        sds ts = edc_datetime_timestamp();
        cJSON_AddStringToObject(msg, "ts", ts ? ts : "");
        sdsfree(ts);
    }
    if (!cJSON_HasObjectItem(msg, "render"))
        cJSON_AddStringToObject(msg, "render", "plain");

    edc_queue_push(&turn->messages, msg);
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

void edc_turn_add_assistant_tool_call(struct edc_turn *turn, const char *call_id, const char *name, cJSON *args)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");

    /* `arguments` is a STRING in the API, not an object. */
    char *args_s = args ? cJSON_PrintUnformatted(args) : NULL;
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "id", call_id ? call_id : "");
    cJSON_AddStringToObject(call, "type", "function");
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", name ? name : "");
    cJSON_AddStringToObject(fn, "arguments", args_s ? args_s : "{}");
    if (args_s)
        cJSON_free(args_s);
    cJSON_AddItemToObject(call, "function", fn);

    cJSON *calls = cJSON_CreateArray();
    cJSON_AddItemToArray(calls, call);
    cJSON_AddItemToObject(msg, "tool_calls", calls);

    /* No "content": edc_turn_add_message fills it with "" (the API accepts an empty
     * content next to tool_calls). */
    edc_turn_add_message(turn, msg);
}

struct edc_turn *edc_turn_new()
{
    struct edc_turn *turn = g_new0(struct edc_turn, 1);
    edc_queue_init(&turn->messages);
    return turn;
}

void edc_turn_free(struct edc_turn *turn)
{
    if (!turn)
    {
        return;
    }

    void *item;
    while ((item = edc_queue_pop(&turn->messages)))
        cJSON_Delete((cJSON *)item);

    edc_queue_destroy(&turn->messages);

    free(turn->task_id);
    free(turn);
}
