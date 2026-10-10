#include "cJSON.h"
#include "edc_token_estimate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 口径测试: 估算公式就是规格。
 *   - 纯文本层: 钉**公式本身** (ASCII /4, 其余字符 1:1), 用精确值。
 *   - 消息层: 钉**结构行为** (结构开销算一次、数组/嵌套里的字符串要走进、
 *     切片要可加), 用关系式而不是魔数 —— 常量将来是可调的, 语义不该变。
 * 与 provider 真值的对拍在 test_group_ctx / test_session_snapshot 那侧。 */

static int failures = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            printf("  ok   %s\n", msg);                                                                                \
        else                                                                                                           \
        {                                                                                                              \
            printf("  FAIL %s\n", msg);                                                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static cJSON *msg(const char *json)
{
    return cJSON_Parse(json);
}

/* n 个 "你" (每个 3 字节, 不掺 NUL)。 */
static void fill_cjk(char *dst, int n)
{
    for (int i = 0; i < n; i++)
        memcpy(dst + i * 3, "你", 3);
    dst[n * 3] = '\0';
}

int main(void)
{
    printf("token estimate: ASCII / 4, everything else 1:1\n");

    /* ── 纯文本: 公式 ── */
    CHECK(edc_token_estimate_text(NULL) == 0, "NULL text costs nothing");
    CHECK(edc_token_estimate_text("") == 0, "empty text costs nothing");
    CHECK(edc_token_estimate_text("aaaaaaaa") == 2, "8 ASCII chars -> 2 tokens");
    CHECK(edc_token_estimate_text("abcde") == 1, "5 ASCII chars -> 1 token (floor)");
    CHECK(edc_token_estimate_text("你好世界") == 4, "4 CJK chars -> 4 tokens (chars/4 would say 1)");
    CHECK(edc_token_estimate_text("hello 世界") == 1 + 2, "mixed: 6 ASCII -> 1, 2 CJK -> 2");
    CHECK(edc_token_estimate_text("é") == 1, "a 2-byte Latin-1 char counts once");
    CHECK(edc_token_estimate_text("🙂") == 1, "a 4-byte emoji counts once (heuristic: emojis really cost more)");
    {
        char cjk[3001];
        fill_cjk(cjk, 1000);
        CHECK(edc_token_estimate_text(cjk) == 1000,
              "1000 CJK chars (3000 bytes) -> 1000 tokens; a flat bytes/4 would say 750");
    }

    /* ── 消息: 结构 ── */
    CHECK(edc_token_estimate_msg(NULL) == 0, "NULL message costs nothing");
    {
        /* 我们自己的元数据 (edc_turn_add_message 挂的 ts / render) 不算:
         * provider 的模板不认识它们, 数了就是每条消息凭空多 ~6 token。 */
        cJSON *plain = msg("{\"role\":\"user\",\"content\":\"aaaa\"}");
        cJSON *ours = msg("{\"role\":\"user\",\"content\":\"aaaa\",\"ts\":\"2026-10-09T06:35:19+08:00\","
                          "\"render\":\"markdown\"}");
        CHECK(edc_token_estimate_msg(ours) == edc_token_estimate_msg(plain),
              "our own keys (ts / render) are not counted");
        cJSON_Delete(plain);
        cJSON_Delete(ours);
    }
    {
        /* 空消息只剩结构开销: 常量被算了一次, 且没把键名算进去 (否则会明显更大)。 */
        cJSON *m = msg("{}");
        const long long overhead = edc_token_estimate_msg(m);
        CHECK(overhead > 0 && overhead <= 16, "an empty message costs only the per-message overhead");
        cJSON_Delete(m);

        cJSON *big = msg("{\"role\":\"user\",\"content\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
        cJSON *bigger = msg("{\"role\":\"user\",\"content\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
        /* 只差 16 个 ASCII 字符 -> 只差 4 token: content 的贡献是 ASCII/4,
         * 与 role / 结构开销无关。 */
        CHECK(edc_token_estimate_msg(bigger) - edc_token_estimate_msg(big) == 4,
              "content adds ASCII/4 on top of the overhead");
        cJSON_Delete(big);
        cJSON_Delete(bigger);
    }
    {
        /* CJK 分支在消息层也成立: 同样字符数, 中文明显更贵。 */
        cJSON *a = msg("{\"role\":\"user\",\"content\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"); /* 30 ASCII */
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"role\":\"user\",\"content\":\"%s\"}", "");
        cJSON_Delete(a);

        char cjk[31 * 3 + 1];
        fill_cjk(cjk, 30);
        char json[256];
        snprintf(json, sizeof(json), "{\"role\":\"user\",\"content\":\"%s\"}", cjk);
        cJSON *cjkm = msg(json);
        cJSON *asciim = msg("{\"role\":\"user\",\"content\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
        CHECK(edc_token_estimate_msg(cjkm) > 3 * edc_token_estimate_msg(asciim) / 4,
              "30 CJK chars cost far more than 30 ASCII chars");
        cJSON_Delete(cjkm);
        cJSON_Delete(asciim);
    }
    {
        /* tool_calls: 参数在**嵌套的对象/数组**里, 不走进去就会把工具轮严重低估。 */
        cJSON *empty = msg("{\"role\":\"assistant\",\"content\":null}");
        cJSON *call = msg("{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"call_1\","
                          "\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":"
                          "\"{\\\"path\\\":\\\"/tmp/a\\\",\\\"limit\\\":200}\"}}]}");
        CHECK(call != NULL && empty != NULL, "tool_calls fixture parses");
        CHECK(edc_token_estimate_msg(call) > edc_token_estimate_msg(empty),
              "tool_call payloads count (id / name / arguments inside arrays and objects)");
        cJSON_Delete(empty);
        cJSON_Delete(call);
    }

    /* ── 切片: 可加 + 夹紧 ── */
    {
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, msg("{\"role\":\"user\",\"content\":\"aaaa\"}"));
        cJSON_AddItemToArray(arr, msg("{\"role\":\"user\",\"content\":\"你好世界\"}"));
        cJSON_AddItemToArray(arr, msg("{\"role\":\"user\",\"content\":\"bbbb\"}"));

        const long long m0 = edc_token_estimate_msgs(arr, 0, 1);
        const long long m1 = edc_token_estimate_msgs(arr, 1, 2);
        const long long m2 = edc_token_estimate_msgs(arr, 2, 3);
        CHECK(m0 > 0 && m1 > m0 && m2 == m0, "per-message: CJK one costs more than the ASCII ones");
        CHECK(edc_token_estimate_msgs(arr, 0, 3) == m0 + m1 + m2, "the whole array is the sum of its messages");
        CHECK(edc_token_estimate_msgs(arr, 1, 3) == m1 + m2, "a slice counts only what is in it");
        CHECK(edc_token_estimate_msgs(arr, 3, 3) == 0, "an empty slice is 0");
        CHECK(edc_token_estimate_msgs(arr, 2, 99) == m2, "to is clamped to the array size");
        CHECK(edc_token_estimate_msgs(NULL, 0, 3) == 0, "NULL array is 0");
        cJSON_Delete(arr);
    }

    printf("=== %s (%d failure%s) ===\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
