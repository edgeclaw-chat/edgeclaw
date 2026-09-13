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

    /* v3: 消息自带产生时间与渲染偏好, 随消息一起持久化 —— 不再用平行的
     * timestamps 队列 (那样重启即丢, 且下标需与 messages 对齐)。 */
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

void edc_turn_add_tool_message(struct edc_turn *turn, const char *tool_call_id, const char *name, const char *content,
                               bool displayed, const char *header, const char *render)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", tool_call_id);
    cJSON_AddStringToObject(msg, "name", name);
    cJSON_AddStringToObject(msg, "content", content);

    /* 显示元数据: 与 ts/render 一样随消息持久化 —— 回放端据此重现实时气泡,
     * 不再拿工具名凑 header。displayed=false 表示"这条工具结果没上屏"。 */
    cJSON_AddBoolToObject(msg, "displayed", displayed);
    if (displayed && header && *header)
        cJSON_AddStringToObject(msg, "header", header);
    if (displayed && render && *render)
    {
        /* 这个键此刻通常还不存在 (edc_turn_add_message 的默认值是稍后才补的),
         * 所以不能只 Replace —— 那会静默丢掉工具声明的 render。 */
        cJSON *r = cJSON_CreateString(render);
        if (!cJSON_ReplaceItemInObjectCaseSensitive(msg, "render", r))
            cJSON_AddItemToObject(msg, "render", r);
    }

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
