/* P3: the overflow classifier. The table is built from real responses (see
 * docs/pi-alignment.md 2.5.1): a real 400 names the window *only* in its message
 * body -- `type` and `code` are both the generic `invalid_request_error` -- and a
 * 429 must never be read as an overflow, because "fold your context" is the wrong
 * advice for a throttled request. Pure function: strings in, verdict out. */

#include "edc_llm_error_kind.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

static int test_failed = 0;
static int test_passed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: %s", msg);                                                                            \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            test_passed++;                                                                                             \
        }                                                                                                              \
    } while (0)

static void check_kind(const char *what, int status, const char *message, edc_llm_error_kind want)
{
    edc_llm_error_kind got = edc_llm_error_classify(status, message);
    if (got != want)
    {
        log_error("    FAIL: %s -- status=%d got=%s want=%s", what, status, edc_llm_error_kind_name(got),
                  edc_llm_error_kind_name(want));
        test_failed++;
        return;
    }
    test_passed++;
}

/* The 400 the real provider returned when the probe pushed 2M tokens at it
 * (verbatim, only the request id elided). */
static const char *kProvider400 =
    "LLM API error (HTTP 400): This model's maximum context length is 1048576 tokens. However, you requested "
    "2011367 tokens (2011366 in the messages, 1 in the completion). Please reduce the length of the messages or "
    "completion. (request_id: 0000)";

int main(void)
{
    log_info("=== test_llm_error_kind ===");

    /* ── 溢出: 实测那条 400, 以及各家换着说法的等价形状 ── */
    check_kind("实测 400 (真机原话)", 400, kProvider400, EDC_LLM_ERR_CONTEXT_OVERFLOW);
    check_kind("全大写也要认", 400, "PLEASE REDUCE THE LENGTH OF THE MESSAGES", EDC_LLM_ERR_CONTEXT_OVERFLOW);
    check_kind("finish_reason 拼进来的字样", 200, "finish_reason=context_length_exceeded", EDC_LLM_ERR_CONTEXT_OVERFLOW);
    check_kind("另一种说法 (prompt is too long)", 400, "prompt is too long: 210000 tokens > 200000 maximum",
               EDC_LLM_ERR_CONTEXT_OVERFLOW);
    /* message 是主键: 状态码不是门 (这是设计, 不是意外) */
    check_kind("500 但文案点名窗口", 500, "maximum context length is 8192", EDC_LLM_ERR_CONTEXT_OVERFLOW);

    /* ── 限流: pi 的显式反例, 必须先于长度判据被排除 ── */
    check_kind("429 限流", 429, "rate limit exceeded: please slow down", EDC_LLM_ERR_RATE_LIMIT);
    check_kind("429 且文案里带 tokens (最像溢出的那种)", 429,
               "too many requests; your request used 200000 tokens, retry in 20s", EDC_LLM_ERR_RATE_LIMIT);
    check_kind("状态码是 400 但文案说限流", 400, "Rate limit reached for requests", EDC_LLM_ERR_RATE_LIMIT);
    check_kind("too many requests", 503, "Too Many Requests", EDC_LLM_ERR_RATE_LIMIT);

    /* ── 传输层: 连状态行都没有 ── */
    check_kind("无状态码 (DNS/TCP/TLS/超时)", 0, "llm actor https error: tcp connect failed: temporary failure",
               EDC_LLM_ERR_TRANSPORT);
    check_kind("无状态码 + 无文案", 0, NULL, EDC_LLM_ERR_TRANSPORT);

    /* ── 鉴权 ── */
    check_kind("401 + provider 原话", 401, "LLM API error (HTTP 401): Authentication Fails, Your api key is invalid",
               EDC_LLM_ERR_AUTH);
    check_kind("403", 403, "forbidden", EDC_LLM_ERR_AUTH);
    check_kind("400 但文案说 key 不对", 400, "invalid api key", EDC_LLM_ERR_AUTH);

    /* ── 其余 ── */
    check_kind("500", 500, "internal server error", EDC_LLM_ERR_OTHER);
    check_kind("400 + 无文案", 400, NULL, EDC_LLM_ERR_OTHER);
    check_kind("NULL message 不崩", 200, NULL, EDC_LLM_ERR_OTHER);

    /* 名字往返 (日志/诊断用; 未知值必须有个名字而不是越界) */
    CHECK(strcmp(edc_llm_error_kind_name(EDC_LLM_ERR_CONTEXT_OVERFLOW), "CONTEXT_OVERFLOW") == 0, "溢出名");
    CHECK(strcmp(edc_llm_error_kind_name(EDC_LLM_ERR_RATE_LIMIT), "RATE_LIMIT") == 0, "限流名");
    CHECK(strcmp(edc_llm_error_kind_name(EDC_LLM_ERR_AUTH), "AUTH") == 0, "鉴权名");
    CHECK(strcmp(edc_llm_error_kind_name(EDC_LLM_ERR_TRANSPORT), "TRANSPORT") == 0, "传输名");
    CHECK(strcmp(edc_llm_error_kind_name(EDC_LLM_ERR_OTHER), "OTHER") == 0, "其余名");
    CHECK(strcmp(edc_llm_error_kind_name((edc_llm_error_kind)999), "OTHER") == 0, "未知值退化为 OTHER");

    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
