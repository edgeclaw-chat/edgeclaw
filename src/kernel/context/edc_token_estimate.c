#include "edc_token_estimate.h"

#include <string.h>

/* Token estimation, pi-shaped: a provider's real usage is always the anchor,
 * this heuristic only fills in what has been appended after it. It exists
 * because "the last reply's usage" answers "how big was that request", never
 * "how big is the context now".
 *
 * 口径: ASCII 按 ~4 字符/token (英文经验值), 其余字符 (CJK 为主) 按 ~1 字符/
 * token。全用 chars/4 会把中文低估 3~4 倍, 而我们的 prompt 大量是中文。
 *
 * 它是启发式, 不是测量: 不得当成 provider 的真值展示 (调用方负责带上估算标记)。 */

/* 每条消息的结构开销: role / name / 分隔符 / JSON 框架 (键名不计, 由它覆盖)。 */
#define EDC_TOKEN_MSG_OVERHEAD 8

static void est_count(const char *s, long long *ascii, long long *wide)
{
    if (!s)
        return;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
    {
        if (*p < 0x80)
        {
            (*ascii)++;
            p++;
        }
        else
        {
            /* 一个非 ASCII 字符 (它的所有 UTF-8 字节算一个字符)。 */
            (*wide)++;
            p++;
            while ((*p & 0xc0) == 0x80)
                p++;
        }
    }
}

/* 走一棵 cJSON 子树的所有**值**, 累加字符串。两件不算:
 *   - 键名: 它们是框架, 已计入每条消息的开销;
 *   - `ts` / `render`: 我们自己挂在消息上的元数据 (edc_turn_add_message 加的),
 *     随消息落盘、也随请求原样发出去, 但 provider 的模板不认识它们, 不会算进
 *     prompt —— 数它们会让每条消息凭空多出 ~6 token。 */
static void est_walk_values(const cJSON *node, long long *ascii, long long *wide)
{
    for (const cJSON *c = node; c; c = c->next)
    {
        if (c->string && (strcmp(c->string, "ts") == 0 || strcmp(c->string, "render") == 0))
            continue;
        if (cJSON_IsString(c))
            est_count(c->valuestring, ascii, wide);
        else if (cJSON_IsArray(c) || cJSON_IsObject(c))
            est_walk_values(c->child, ascii, wide);
    }
}

long long edc_token_estimate_text(const char *s)
{
    if (!s)
        return 0;
    long long ascii = 0, wide = 0;
    est_count(s, &ascii, &wide);
    return ascii / 4 + wide;
}

long long edc_token_estimate_msg(const cJSON *msg)
{
    if (!msg)
        return 0;
    long long ascii = 0, wide = 0;
    est_walk_values(msg->child, &ascii, &wide);
    return EDC_TOKEN_MSG_OVERHEAD + ascii / 4 + wide;
}

long long edc_token_estimate_msgs(const cJSON *arr, size_t from, size_t to)
{
    if (!arr || to <= from)
        return 0;

    long long total = 0;
    size_t i = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        if (i >= to)
            break;
        if (i >= from)
            total += edc_token_estimate_msg(item);
        i++;
    }
    return total;
}
