#include "edc_actor.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_llm_actor.h"
#include "sds.h"
#include "uv.h"
#include <stdio.h>
#include <string.h>

/* 手动测试（网络 + 真 provider，故 tags=manual）：钉住"llm_actor 把 HTTP 状态码
 * 与 provider 自己的错误文本透出来"这条链。
 *
 *   llhttp 状态行 → edc_https_request_get_status() → resp->http_status
 *                 → resp->error->message（不再是整坨 JSON）
 *
 * 拿 401（鉴权失败）验证：故意用无效 key —— provider 在鉴权前不会校验别的，
 * 所以这条既确定、又不花 token、又不依赖用户真 key。
 * 溢出的 400（context overflow）与限流 429 走同一条路，只是 status 不同。
 *
 * 跑法：bazel test //tests:test_llm_actor_http_status --test_output=all
 * 依赖：~/.edgeclaw/workspace/ssl/cacert.pem（TLS 校验用，与真 app 同源）。 */

#define API_URL "https://api.deepseek.com/chat/completions"
#define MODEL "deepseek-v4-flash"
#define BOGUS_KEY "sk-000000000000000000000000000000000000"

static uv_sem_t done_sem;
static int got_status = -1;
static char got_error[512];

static void observe_llm_handler(edc_actor_t *actor, edc_actor_msg_t *message)
{
    if (message->type == BC_ACTOR_MSG_LLM_RESPONSE)
    {
        struct edc_llm_actor_resp *resp = message->data;
        got_status = resp->http_status;
        if (resp->error && resp->error->message)
        {
            snprintf(got_error, sizeof(got_error), "%s", resp->error->message);
        }
        uv_sem_post(&done_sem);
    }
    edc_actor_msg_free(message);
}

int main(void)
{
    int failures = 0;

    /* 手工装配最小 config：不加载 config.json ⇒ 不需要 EDGECLAW_MASTER_KEY。 */
    struct edc_config conf;
    memset(&conf, 0, sizeof(conf));
    sds home = edc_homedir();
    conf.workspace = edc_path_join(home, ".edgeclaw", "workspace", NULL);
    conf.llm_api_conf.api_url = sdsnew(API_URL);
    conf.llm_api_conf.model = sdsnew(MODEL);
    conf.llm_api_conf.api_key = sdsnew(BOGUS_KEY);

    edc_actor_t *llm_actor = edc_llm_actor_new(&conf);
    edc_actor_t *observer = edc_actor_new("edc_http_status_observer", observe_llm_handler, NULL);

    edc_actor_runner_t *runner = edc_actor_runner_new();
    edc_actor_runner_add(runner, llm_actor);
    edc_actor_runner_add(runner, observer);
    edc_actor_runner_start(runner);

    cJSON *msgs = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", "ping");
    cJSON_AddItemToArray(msgs, msg);

    struct edc_llm_actor_req *req = edc_llm_actor_req_new(msgs, NULL, NULL);
    edc_actor_send(llm_actor, edc_actor_msg_new(BC_ACTOR_MSG_LLM_REQUEST, observer, NULL, req,
                                                (edc_actor_data_free_fn)edc_llm_actor_req_free));

    uv_sem_init(&done_sem, 0);
    uv_sem_wait(&done_sem); /* 网络失败会在这里挂住 → bazel 超时（与 test_llm_actor 同款） */

    printf("http_status = %d\n  error = %s\n", got_status, got_error);

    if (got_status != 401)
    {
        fprintf(stderr, "FAIL: 状态码 %d, 期望 401\n", got_status);
        failures++;
    }
    /* 断言必须能把"读到了 provider 的原话"与"退回整坨 JSON"分开：后者虽然也含
     * HTTP 401（两条分支都带状态码），但文本完全不同 —— 它会把 error 对象的键名
     * 一起打出来。用前缀精确匹配。 */
    if (strstr(got_error, "LLM API error (HTTP 401): Authentication") == NULL)
    {
        fprintf(stderr, "FAIL: 错误文本不是 \"LLM API error (HTTP 401): <provider 原话>\"\n");
        failures++;
    }
    fprintf(stderr, failures ? "=== %d 处失败 ===\n" : "=== 全部通过 ===\n", failures);

    edc_actor_runner_free(runner);
    edc_actor_free(observer);
    edc_actor_free(llm_actor);
    uv_sem_destroy(&done_sem);
    sdsfree(conf.workspace);
    sdsfree(conf.llm_api_conf.api_url);
    sdsfree(conf.llm_api_conf.model);
    sdsfree(conf.llm_api_conf.api_key);
    return failures ? 1 : 0;
}
