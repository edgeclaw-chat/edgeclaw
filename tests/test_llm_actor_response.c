/* test_llm_actor_response: what the actor does with the provider's answer.
 *
 * This is the failure-path suite the actor was missing. `edc_llm_actor.c` sat at 58% lines
 * and **33.5% branches** because everything after the bytes arrive was unreachable from a
 * test: `edc_llm_actor_parse_resp()` is static, driven only by an actor message that is only
 * sent when a real HTTPS response lands. So the parser -- the code that decides whether the
 * user is told "context overflow" or "your key is wrong", and that turns `finish_reason` and
 * `usage` into numbers the kernel bills against -- had no test at all.
 *
 * tests/tls_stub.h closes that gap honestly: a loopback TLS server with a per-run
 * certificate, so the real client, the real llhttp status line and the real parser all run.
 * Nothing is stubbed between the socket and the code under test.
 *
 * The cases are chosen around the decisions that are expensive to get wrong:
 *
 *   - a 4xx WITH `error.message` must surface the provider's own sentence, because that is
 *     what the error classifier reads ("maximum context length") -- the raw-JSON dump that
 *     used to go out here hid exactly those words;
 *   - a 4xx WITHOUT a usable message must still produce an error, never a crash: one "return
 *     false" in the parser once failed to set `resp->error`, and the caller's log line
 *     dereferenced NULL, turning a provider quirk into a silent segfault;
 *   - a body that is not JSON at all;
 *   - every `finish_reason` the kernel distinguishes, including the ones it does not know;
 *   - `usage`, including the two places providers disagree about the cached-prompt count and
 *     the reconstruction of a missing `total_tokens`;
 *   - tool calls, whose `arguments` arrive as a JSON *string* that has to be parsed again.
 */

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_exec_io.h"
#include "edc_llm_actor.h"
#include "edc_llm_error_kind.h"
#include "sds.h"
#include "uv.h"

#include "tls_stub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) printf("\n-- %s\n", name)

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed++; } \
    else { printf("    ok: "); printf(__VA_ARGS__); printf("\n"); g_passed++; } \
} while (0)

/* ── harness ───────────────────────────────────────────────────────────────── */

static char g_dir[512];
static char g_cert[600];
static char g_key[600];

static void make_dir(void)
{
    /* $TEST_TMPDIR is Bazel's per-test scratch; /tmp on this project has filled up before and
     * produced EDQUOT that looked like a code failure. */
    const char *base = getenv("TEST_TMPDIR");
    if (!base || !*base)
        base = "/tmp";
    snprintf(g_dir, sizeof(g_dir), "%s/ec-llmresp-XXXXXX", base);
    if (!mkdtemp(g_dir))
    {
        perror("mkdtemp");
        abort();
    }
}

static void drop_dir(void)
{
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "    (warning: could not remove %s)\n", g_dir);
}

/* One request against a stub that answers with `raw`. Fills the observer's slot with the
 * response the actor produced. Returns false when the callback never fired. */
struct observed
{
    uv_sem_t done;
    bool fired;
    bool timed_out;
    int http_status;
    int reason;
    char error[1024];
    long long prompt_tokens;
    long long completion_tokens;
    long long total_tokens;
    long long cached_tokens;
    int tool_calls;
    char tool_name[128];
    char tool_args[512];
    char content[512];
};

static void observer_handler(edc_actor_t *actor, edc_actor_msg_t *message)
{
    if (message->type != BC_ACTOR_MSG_LLM_RESPONSE)
        return;
    struct observed *o = actor->state;
    if (!o)
        return;
    struct edc_llm_actor_resp *resp = message->data;
    o->fired = true;
    o->http_status = resp->http_status;
    o->reason = (int)resp->reason;
    if (resp->error && resp->error->message)
        snprintf(o->error, sizeof(o->error), "%s", resp->error->message);
    o->prompt_tokens = (long long)resp->usage.prompt_tokens;
    o->completion_tokens = (long long)resp->usage.completion_tokens;
    o->total_tokens = (long long)resp->usage.total_tokens;
    o->cached_tokens = (long long)resp->usage.cached_tokens;
    if (resp->content)
        snprintf(o->content, sizeof(o->content), "%s", resp->content);
    if (resp->tool_calls && utarray_len(resp->tool_calls) > 0)
    {
        o->tool_calls = (int)utarray_len(resp->tool_calls);
        struct edc_llm_chat_tool_call *tc =
            (struct edc_llm_chat_tool_call *)utarray_eltptr(resp->tool_calls, 0);
        if (tc)
        {
            if (tc->function.name)
                snprintf(o->tool_name, sizeof(o->tool_name), "%s", tc->function.name);
            /* `arguments` arrives from the provider as a JSON *string* which the parser
             * immediately parses into an object (cJSON_Parse), so what is stored is the
             * object, not the string. Serialize it back to check the payload survived. */
            if (tc->function.arguments)
            {
                char *printed = cJSON_PrintUnformatted(tc->function.arguments);
                if (printed)
                {
                    snprintf(o->tool_args, sizeof(o->tool_args), "%s", printed);
                    cJSON_free(printed);
                }
            }
            else
            {
                snprintf(o->tool_args, sizeof(o->tool_args), "(null)");
            }
        }
    }
    uv_sem_post(&o->done);
}

/* Watchdog: libuv (1.52 here) has no uv_sem_timedwait, so a plain wait would hang the whole
 * test binary -- which is exactly what the first version did (a 300s Bazel timeout with no
 * output at all, the least debuggable failure there is). This thread releases the wait after
 * a deadline and flags why, so a stuck actor fails one case instead of the run. */
static void watchdog_thread(void *arg)
{
    struct observed *o = arg;
    for (int i = 0; i < 400; i++) /* 400 * 50ms = 20s */
    {
        uv_sleep(50);
        if (o->fired)
            return;
    }
    o->timed_out = true;
    uv_sem_post(&o->done);
}

/* Drive one request at a stub answering `raw`. The actor needs its trust anchor at
 * <workspace>/ssl/cacert.pem -- that is where edc_llm_actor_new() looks. */
static bool ask(const char *raw, struct observed *o)
{
    memset(o, 0, sizeof(*o));
    uv_sem_init(&o->done, 0);

    struct tls_stub stub;
    if (tls_stub_start(&stub, g_cert, g_key, raw) != 0)
    {
        fprintf(stderr, "    (could not start the TLS stub)\n");
        return false;
    }

    char api_url[256];
    snprintf(api_url, sizeof(api_url), "https://localhost:%d/chat/completions", stub.port);

    struct edc_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.workspace = sdsnew(g_dir);
    cfg.llm_api_conf.api_url = sdsnew(api_url);
    cfg.llm_api_conf.api_key = sdsnew("test-key");
    cfg.llm_api_conf.model = sdsnew("test-model");

    edc_actor_t *llm = edc_llm_actor_new(&cfg);
    edc_actor_t *obs = edc_actor_new("llm-response-observer", observer_handler, o);

    /* Actors only run when a runner owns them: without this the request sits in the mailbox
     * and the wait would run to its deadline (which is exactly how the first version of this
     * test hung). */
    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, llm);
    edc_actor_runner_add(runner, obs);
    edc_actor_runner_start(runner);

    struct edc_llm_actor_req *req = edc_llm_actor_req_new(NULL, NULL);
    edc_actor_send(llm, edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, obs, NULL, req,
                                          (edc_actor_data_free_fn)edc_llm_actor_req_free));

    /* Wait on the semaphore the observer posts; the runner's thread drives everything, so
     * this thread only waits. The watchdog is what keeps a stuck actor from hanging the whole
     * binary (there is no uv_sem_timedwait in this libuv). */
    uv_thread_t dog;
    if (uv_thread_create(&dog, watchdog_thread, o) != 0)
        fprintf(stderr, "    (no watchdog could be started)\n");
    uv_sem_wait(&o->done);

    edc_actor_runner_stop(runner);
    edc_actor_runner_free(runner);
    tls_stub_stop(&stub);
    uv_thread_join(&dog);
    edc_actor_free(obs);
    edc_llm_actor_free(llm);
    sdsfree(cfg.workspace);
    sdsfree(cfg.llm_api_conf.api_url);
    sdsfree(cfg.llm_api_conf.api_key);
    sdsfree(cfg.llm_api_conf.model);
    uv_sem_destroy(&o->done);
    return o->fired;
}

/* ── 4xx / 5xx ─────────────────────────────────────────────────────────────── */

static void test_error_body_message_is_surfaced(void)
{
    TEST("a 400 with error.message surfaces the provider's own sentence");
    /* The verbatim shape the real provider returned when the probe pushed 2M tokens at it.
     * What matters is that "maximum context length" survives into resp->error->message --
     * that is the string the error classifier keys on. */
    const char *body =
        "{\"error\":{\"message\":\"This model's maximum context length is 1048576 tokens. "
        "However, you requested 2011367 tokens\",\"type\":\"invalid_request_error\"}}";
    char raw[1024];
    tls_stub_response(raw, sizeof(raw), "400 Bad Request", body);

    struct observed o;
    CHECK(ask(raw, &o), "the actor answered");
    CHECK(o.http_status == 400, "http_status is 400, got %d", o.http_status);
    CHECK(o.error[0] != '\0', "an error was set");
    CHECK(strstr(o.error, "maximum context length") != NULL,
          "the provider's sentence is in the error: %.80s", o.error);
}

static void test_rate_limit_is_not_an_overflow(void)
{
    TEST("a 429 that talks about tokens is still a rate limit");
    /* The counterexample the classifier exists for: the body mentions tokens, so a naive
     * implementation would call it an overflow and tell the user to fold their context. */
    const char *body =
        "{\"error\":{\"message\":\"Rate limit reached: your request used 200000 tokens, retry in 20s\"}}";
    char raw[512];
    tls_stub_response(raw, sizeof(raw), "429 Too Many Requests", body);

    struct observed o;
    CHECK(ask(raw, &o), "the actor answered");
    CHECK(o.http_status == 429, "http_status is 429, got %d", o.http_status);
    CHECK(strstr(o.error, "Rate limit") != NULL, "the throttle sentence survived: %.80s", o.error);
    /* The classifier must agree with itself here. */
    CHECK(edc_llm_error_classify(o.http_status, o.error) == EDC_LLM_ERR_RATE_LIMIT,
          "and it classifies as RATE_LIMIT, not an overflow");
}

static void test_error_without_message_still_fails_cleanly(void)
{
    TEST("a 500 with no usable error.message still produces an error, and does not crash");
    /* The regression this guards: one `return false` in the parser did not set resp->error,
     * and the caller's log line dereferenced NULL -- a provider quirk became a segfault with
     * no log line. Any answer here must end with a non-empty error. */
    const char *body = "{\"unexpected\":\"shape\",\"no\":\"choices\"}";
    char raw[512];
    tls_stub_response(raw, sizeof(raw), "500 Internal Server Error", body);

    struct observed o;
    CHECK(ask(raw, &o), "the actor answered");
    CHECK(o.http_status == 500, "http_status is 500, got %d", o.http_status);
    CHECK(o.error[0] != '\0', "an error was set even though the body had no error object: %.80s",
          o.error);
    CHECK(edc_llm_error_classify(o.http_status, o.error) == EDC_LLM_ERR_OTHER,
          "and it classifies as OTHER");
}

static void test_non_json_body(void)
{
    TEST("a body that is not JSON at all is reported, not parsed blindly");
    char raw[512];
    tls_stub_response(raw, sizeof(raw), "502 Bad Gateway", "<html>bad gateway</html>");

    struct observed o;
    CHECK(ask(raw, &o), "the actor answered");
    CHECK(o.error[0] != '\0', "an error was set for an unparseable body: %.100s", o.error);
}

/* ── finish_reason ─────────────────────────────────────────────────────────── */

/* A success body with one choice and the given finish_reason. */
static void ok_body(char *out, size_t cap, const char *finish_reason)
{
    snprintf(out, cap,
             "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"done\"},"
             "\"finish_reason\":%s}]}",
             finish_reason);
}

static void test_finish_reasons(void)
{
    TEST("every finish_reason the kernel distinguishes, plus an unknown one");

    struct
    {
        const char *json;
        int want;
        const char *what;
    } cases[] = {
        {"\"stop\"", BC_LLM_FINISH_REASON_STOP, "stop"},
        {"\"tool_calls\"", BC_LLM_FINISH_REASON_TOOL_CALL, "tool_calls"},
        {"\"length\"", BC_LLM_FINISH_REASON_LENGTH, "length"},
        {"\"sensitive\"", BC_LLM_FINISH_REASON_SENSITIVE, "sensitive"},
        {"\"network_error\"", BC_LLM_FINISH_REASON_NETWORK_ERROR, "network_error"},
        {"\"context_length_exceeded\"", BC_LLM_FINISH_REASON_CONTEXT_EXCEEDED, "context_length_exceeded"},
        {"\"something_new\"", BC_LLM_FINISH_REASON_UNKNOWN, "an unknown reason"},
        {"null", BC_LLM_FINISH_REASON_UNKNOWN, "a null reason"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char body[512], raw[768];
        ok_body(body, sizeof(body), cases[i].json);
        tls_stub_response(raw, sizeof(raw), "200 OK", body);

        struct observed o;
        if (!ask(raw, &o))
        {
            CHECK(false, "%s: the actor answered", cases[i].what);
            continue;
        }
        CHECK(o.reason == cases[i].want, "%s maps to %d, got %d", cases[i].what, cases[i].want,
              o.reason);
    }
}

/* ── usage ─────────────────────────────────────────────────────────────────── */

static void test_usage_is_read(void)
{
    TEST("usage: the four counters are read from the body");
    const char *body =
        "{\"choices\":[{\"index\":0,\"message\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1200,\"completion_tokens\":340,\"total_tokens\":1540}}";
    char raw[512];
    tls_stub_response(raw, sizeof(raw), "200 OK", body);

    struct observed o;
    CHECK(ask(raw, &o), "the actor answered");
    CHECK(o.prompt_tokens == 1200, "prompt_tokens, got %lld", o.prompt_tokens);
    CHECK(o.completion_tokens == 340, "completion_tokens, got %lld", o.completion_tokens);
    CHECK(o.total_tokens == 1540, "total_tokens, got %lld", o.total_tokens);
}

static void test_usage_total_is_reconstructed(void)
{
    TEST("usage: a missing total_tokens is reconstructed from the parts");
    const char *body =
        "{\"choices\":[{\"index\":0,\"message\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":25}}";
    char raw[512];
    tls_stub_response(raw, sizeof(raw), "200 OK", body);

    struct observed o;
    CHECK(ask(raw, &o), "the actor answered");
    CHECK(o.total_tokens == 125, "total = prompt + completion = 125, got %lld", o.total_tokens);
}

static void test_cached_tokens_both_shapes(void)
{
    TEST("usage: the cached count is accepted in either provider's shape");

    /* DeepSeek: top level. */
    {
        const char *body =
            "{\"choices\":[{\"index\":0,\"message\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
            "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":10,\"total_tokens\":110,"
            "\"prompt_cache_hit_tokens\":64}}";
        char raw[600];
        tls_stub_response(raw, sizeof(raw), "200 OK", body);
        struct observed o;
        if (ask(raw, &o))
            CHECK(o.cached_tokens == 64, "top-level prompt_cache_hit_tokens, got %lld", o.cached_tokens);
        else
            CHECK(false, "the actor answered (DeepSeek shape)");
    }

    /* OpenAI: nested. */
    {
        const char *body =
            "{\"choices\":[{\"index\":0,\"message\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
            "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":10,\"total_tokens\":110,"
            "\"prompt_tokens_details\":{\"cached_tokens\":48}}}";
        char raw[600];
        tls_stub_response(raw, sizeof(raw), "200 OK", body);
        struct observed o;
        if (ask(raw, &o))
            CHECK(o.cached_tokens == 48, "nested prompt_tokens_details.cached_tokens, got %lld",
                  o.cached_tokens);
        else
            CHECK(false, "the actor answered (OpenAI shape)");
    }
}

/* ── tool calls ───────────────────────────────────────────────────────────── */

static void test_tool_call_arguments_are_parsed(void)
{
    TEST("tool calls: the name and the arguments string are recovered");
    /* `arguments` arrives as a JSON *string* that has to be parsed a second time -- the shape
     * every provider uses and the one place a tool-calling agent silently loses its input. */
    const char *body =
        "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":null,"
        "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"a.c\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":20,\"total_tokens\":70}}";
    char raw[900];
    tls_stub_response(raw, sizeof(raw), "200 OK", body);

    struct observed o;
    CHECK(ask(raw, &o), "the actor answered");
    CHECK(o.tool_calls == 1, "one tool call was recorded, got %d", o.tool_calls);
    CHECK(strcmp(o.tool_name, "read_file") == 0, "the name survived, got '%s'", o.tool_name);
    CHECK(strstr(o.tool_args, "a.c") != NULL, "the arguments still contain the payload: %.80s",
          o.tool_args);
}

int main(void)
{
    printf("test_llm_actor_response\n");

    make_dir();
    snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", g_dir);
    snprintf(g_key, sizeof(g_key), "%s/key.pem", g_dir);
    if (tls_stub_cert(g_cert, g_key) != 0)
    {
        fprintf(stderr, "could not generate the test certificate\n");
        return 1;
    }

    /* The actor reads its trust anchor from <workspace>/ssl/cacert.pem. */
    {
        char ssl_dir[600];
        snprintf(ssl_dir, sizeof(ssl_dir), "%s/ssl", g_dir);
        char cmd[700];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && cp '%s' '%s/cacert.pem'", ssl_dir, g_cert, ssl_dir);
        if (system(cmd) != 0)
        {
            fprintf(stderr, "could not install the trust anchor\n");
            return 1;
        }
    }

    test_error_body_message_is_surfaced();
    test_rate_limit_is_not_an_overflow();
    test_error_without_message_still_fails_cleanly();
    test_non_json_body();
    test_finish_reasons();
    test_usage_is_read();
    test_usage_total_is_reconstructed();
    test_cached_tokens_both_shapes();
    test_tool_call_arguments_are_parsed();

    drop_dir();
    printf("\n=== 测试结果: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
