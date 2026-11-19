#include "edc_chat_bubble_page.hpp"

#include "cJSON.h"

#include <cstdlib>
#include <cinttypes>

namespace edc
{

std::string EncodeChatBubblePageRequest(const ChatBubblePageRequest &request)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", request.session_id.c_str());
    cJSON_AddNumberToObject(root, "before_seq", (double)request.before_seq);
    cJSON_AddNumberToObject(root, "limit", request.limit);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed)
        return {};
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

bool DecodeChatBubblePageRequest(const char *json, ChatBubblePageRequest *out)
{
    if (!json || !*json || !out)
        return false;
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return false;
    const cJSON *sid = cJSON_GetObjectItem(root, "session_id");
    const cJSON *before = cJSON_GetObjectItem(root, "before_seq");
    const cJSON *limit = cJSON_GetObjectItem(root, "limit");
    const bool ok = cJSON_IsString(sid) && sid->valuestring && *sid->valuestring &&
                    cJSON_IsNumber(before) && cJSON_IsNumber(limit) && limit->valueint > 0;
    if (ok)
    {
        out->session_id = sid->valuestring;
        const double raw = before->valuedouble;
        /* Clamp instead of relying on a giant sentinel that cannot survive a
         * double round-trip. Non-positive = newest page. */
        out->before_seq = (raw > 0 && raw < 9.0e18) ? (int64_t)raw : 0;
        out->limit = limit->valueint;
    }
    cJSON_Delete(root);
    return ok;
}

std::string EncodeChatBubblePageReply(const char *session_id, int64_t before_seq,
                                      const char *const *lines, const int64_t *seqs,
                                      int n, bool has_more)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id ? session_id : "");
    cJSON_AddNumberToObject(root, "before_seq", (double)before_seq);
    cJSON_AddBoolToObject(root, "has_more", has_more);
    cJSON *arr = cJSON_AddArrayToObject(root, "rows");
    for (int i = 0; i < n; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seq", (double)seqs[i]);
        cJSON_AddStringToObject(item, "line", lines[i] ? lines[i] : "");
        cJSON_AddItemToArray(arr, item);
    }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed)
        return {};
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

bool DecodeChatBubblePageReply(const char *header, const char *json, ChatBubblePageReply *out)
{
    if (!json || !*json || !out)
        return false;
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return false;

    out->session_id = header ? header : "";
    out->rows.clear();
    out->has_more = false;
    out->before_seq = 0;

    const cJSON *before = cJSON_GetObjectItem(root, "before_seq");
    const cJSON *more = cJSON_GetObjectItem(root, "has_more");
    const cJSON *arr = cJSON_GetObjectItem(root, "rows");
    if (cJSON_IsNumber(before))
    {
        const double raw = before->valuedouble;
        out->before_seq = (raw > 0 && raw < 9.0e18) ? (int64_t)raw : 0;
    }
    if (cJSON_IsBool(more))
        out->has_more = cJSON_IsTrue(more);
    if (cJSON_IsArray(arr))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, arr)
        {
            const cJSON *seq = cJSON_GetObjectItem(item, "seq");
            const cJSON *line = cJSON_GetObjectItem(item, "line");
            if (!cJSON_IsNumber(seq) || !cJSON_IsString(line) || !line->valuestring)
                continue;
            out->rows.push_back({(int64_t)seq->valuedouble, line->valuestring});
        }
    }
    cJSON_Delete(root);
    return true;
}

} // namespace edc
