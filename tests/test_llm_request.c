/* 请求装配器的下线: `turn_begin`。
 *
 * P2a-4b 让"折叠可以切在**在飞 turn 的内部**"成立 —— 落地后 active_turn 带上前缀,
 * 于是这条请求必须从 `turn->folded_prefix` 之后开始发 (前缀已经在组记忆里了, 再发一遍
 * 就是双份)。生产者是 `edc_llm_request_send`; 消费者是 llm actor 的发包循环
 * (`req->turn->head + req->turn_begin`)。本文件钉的就是生产者那一侧, 以及"没有前缀时
 * 必须整条发"的另一半 —— 这条边此前没有任何测试 (老实现里 turn_begin 恒 0)。
 *
 * Harness: 真 actor (llm 侧) 加到 runner 上, 但**不起 runner 线程**, 本线程
 * `uv_run(..., UV_RUN_NOWAIT)` 抽循环 ⇒ handler 内联执行、断言零锁 (与
 * test_toolbox_request.c 同一配方)。捕获器把 req 从消息里摘下来自己持有。 */

#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_llm_actor.h"
#include "edc_group_fold.h"
#include "edc_llm_request.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include <cJSON.h>
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

/* Stub for _run_loop pulled in by edc_commands (transitively linked). */
volatile sig_atomic_t _run_loop = 1;

/* ══════════════════════════════════════════════════════════════
 *  Test framework
 *  ══════════════════════════════════════════════════════════════ */

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        log_info("  TEST: %s", name);                                                                                  \
    } while (0)
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

/* ══════════════════════════════════════════════════════════════
 *  Capture side
 *  ══════════════════════════════════════════════════════════════ */

static struct edc_llm_actor_req *g_req; /* 测试自己持有 (摘下来, 不让 actor 释放) */
static int g_req_msgs;

static void llm_capture_handler(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    (void)actor;
    if (msg->type == BC_ACTOR_MSG_LLM_REQUEST && msg->data)
    {
        g_req_msgs++;
        g_req = msg->data;
        msg->data = NULL;
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Harness
 *  ══════════════════════════════════════════════════════════════ */

struct harness
{
    struct edc_config *cfg;
    struct edc_scheduler *sched;
    struct edc_session *sess;
    edc_actor_runner_t *runner;
    edc_actor_t *llm_actor;
};

static void close_handle_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, NULL);
}

static void install_sessions(struct edc_scheduler *sched, struct edc_session *sess)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}

static void harness_up(struct harness *h)
{
    h->cfg = g_new0(struct edc_config, 1);
    h->cfg->workspace = sdsnew("/tmp/test_llm_request_ws");
    h->sched = edc_scheduler_new(h->cfg);
    h->sess = edc_session_new(h->cfg, "sess-req", "Req");
    install_sessions(h->sched, h->sess);

    h->llm_actor = edc_llm_actor_new(h->cfg);
    h->llm_actor->handler = llm_capture_handler;
    h->runner = edc_actor_runner_new();
    edc_actor_runner_add(h->runner, h->llm_actor); /* actor->loop 得存在, 否则 send() 直接丢 */

    g_req = NULL;
    g_req_msgs = 0;
}

static void pump(struct harness *h)
{
    for (int i = 0; i < 4; i++)
        uv_run(&h->runner->loop, UV_RUN_NOWAIT);
}

static void harness_down(struct harness *h)
{
    uv_walk(&h->runner->loop, close_handle_cb, NULL);
    uv_run(&h->runner->loop, UV_RUN_DEFAULT);
    if (g_req)
        edc_llm_actor_req_free(g_req);
    g_req = NULL;
    edc_llm_actor_free(h->llm_actor);
    edc_actor_runner_free(h->runner);
    edc_session_free(h->sess);
    edc_scheduler_free(h->sched);
    edc_config_free(h->cfg);
}

/* ── fixtures ─────────────────────────────────────────────── */

static struct edc_task_group *make_group(struct harness *h, struct edc_task **out_task, const char *sid)
{
    struct edc_task *leader = edc_task_new("1", sid, "1", "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(h->sess, g);
    edc_task_await_exec(leader);
    edc_task_mark_completed(leader);
    leader->completion_seq = 1;
    if (out_task)
        *out_task = leader;
    return g;
}

/* 组历史里放一条已提交的成员消息 (只读引用, 数组负责生命周期以外的事)。 */
static void history_add_user(struct edc_task_group *g, const char *text)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    edc_task_group_history_add_owned(g, m);
}

/* 当前 turn: input + 两条后续消息 (模拟工具循环里长起来的 turn)。 */
static struct edc_turn *attach_turn(struct harness *h, struct edc_task *t, const char *input)
{
    edc_scheduler_attach_input(h->sched, t, input);
    struct edc_turn *turn = t->active_turn;
    for (int i = 1; i <= 2; i++)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "turn msg %d", i);
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", buf);
        edc_turn_add_message(turn, m);
    }
    return turn;
}

/* ══════════════════════════════════════════════════════════════
 *  Case 1: 折在 turn 内部 ⇒ 请求从 turn_begin 之后开始
 *  ══════════════════════════════════════════════════════════════ */

static void test_turn_begin_honours_folded_prefix(void)
{
    TEST("turn_begin: active_turn 的前缀已进记忆 ⇒ 请求从它之后开始发");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-req");
    history_add_user(g, "already committed member message");

    struct edc_turn *turn = attach_turn(&h, leader, "current input");
    CHECK(edc_queue_length(&turn->messages) == 3, "当前 turn 有 3 条");

    /* 折叠切在这个 turn 内部: 前缀 1 条进了组记忆。 */
    turn->folded_prefix = 1;
    edc_llm_request_send(h.sched, leader, NULL, h.llm_actor, NULL);
    pump(&h);

    CHECK(g_req_msgs == 1, "请求真发出去了");
    if (g_req)
    {
        CHECK(g_req->turn_begin == 1, "turn_begin = 折叠前缀 (前缀不再重复发送)");
        CHECK(g_req->turn == &turn->messages, "turn 指向当前 turn 的消息队列");
    }
    /* 组历史那一条 (未折叠) 必须在, 折掉的那条 turn 消息由 turn_begin 拦。 */
    int found_member_msg = 0;
    if (g_req && g_req->history)
    {
        for (int i = 0; i < cJSON_GetArraySize(g_req->history); i++)
        {
            cJSON *m = cJSON_GetArrayItem(g_req->history, i);
            cJSON *c = cJSON_GetObjectItem(m, "content");
            if (c && cJSON_IsString(c) && strcmp(c->valuestring, "already committed member message") == 0)
                found_member_msg = 1;
        }
    }
    CHECK(found_member_msg, "已提交的成员消息仍在请求里 (只有前缀被折)");

    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 2: 没有前缀 ⇒ 整条发 (turn_begin 必须是 0)
 *  ══════════════════════════════════════════════════════════════ */

static void test_turn_begin_zero_without_prefix(void)
{
    TEST("turn_begin: 没折过就是 0 (整条 turn 照发)");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-req");
    (void)g;

    struct edc_turn *turn = attach_turn(&h, leader, "current input");
    CHECK(turn->folded_prefix == 0, "新 turn 没有前缀");

    edc_llm_request_send(h.sched, leader, NULL, h.llm_actor, NULL);
    pump(&h);

    CHECK(g_req_msgs == 1, "请求发出去了");
    if (g_req)
        CHECK(g_req->turn_begin == 0, "turn_begin = 0 (整条 turn 从头发)");

    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 3: 摘要调用的 max_tokens (P2b-2)
 *  ══════════════════════════════════════════════════════════════ */

static void test_summary_request_is_capped(void)
{
    TEST("max_tokens: 折叠的摘要请求带上限, 普通回复一个字段都不多发");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-req");
    (void)g;

    /* 普通任务: 不带 max_tokens (回复从不封顶)。 */
    attach_turn(&h, leader, "current input");
    edc_llm_request_send(h.sched, leader, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 1 && g_req != NULL, "普通请求发出去了");
    if (g_req)
        CHECK(g_req->max_tokens == 0, "普通任务: max_tokens = 0 (请求体里没有这个字段)");
    if (g_req)
    {
        edc_llm_actor_req_free(g_req);
        g_req = NULL;
    }

    /* 折叠任务 (sink = GROUP_MEMORY): 摘要必须装得进 reserve。 */
    struct edc_task *fold = edc_scheduler_spawn_system_task(h.sched, h.sess, "fold group memory");
    CHECK(fold != NULL && fold->sink == EDC_SINK_GROUP_MEMORY, "拿到一个折叠任务");
    edc_scheduler_attach_input(h.sched, fold, "summarize this");
    edc_llm_request_send(h.sched, fold, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 2 && g_req != NULL, "摘要请求发出去了");
    if (g_req)
    {
        CHECK(g_req->max_tokens == EDC_FOLD_SUMMARY_MAX_TOKENS, "摘要请求带 max_tokens = 0.8 x reserve");
        CHECK(g_req->max_tokens == 13107, "而且就是 13107 (跟 pi: min(0.8 x reserve, 模型上限))");
        CHECK(g_req->tools == NULL, "摘要调用依旧不带工具定义 (对照, 不许退步)");
    }

    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 4: 折叠请求是**自足**的 (折叠说明 + 折叠 prompt, 别无他物)
 *  ══════════════════════════════════════════════════════════════ */

static void test_fold_request_is_self_contained(void)
{
    TEST("折叠请求: 只有折叠说明 + 折叠 prompt; 重启后也不许混进主提示词");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-fold-req");
    (void)g;

    struct edc_task *fold = edc_scheduler_spawn_system_task(h.sched, h.sess, "fold group memory");
    CHECK(fold != NULL, "起一个折叠任务");
    struct edc_task_group *sys = edc_session_find_system_group(h.sess);
    CHECK(sys != NULL, "它进了系统组");

    /* 模拟重启: 组的 history **不落盘** ⇒ 加载器读回的是空 history, 并对每个组跑一次
     * rebuild (deser_rebuild_group_history) —— 这一跑就是本用例的病根: 系统组会因此
     * 拿到一条主提示词。 */
    if (sys)
    {
        cJSON_Delete(sys->history);
        sys->history = cJSON_CreateArray();
        edc_task_group_rebuild_history(sys);
        /* 显式摆出**修复前重启后**的真形状: 那个槽位里躺着主提示词 (当时的加载期
         * 就是这么填的, 实测确认过)。修复后折叠请求不读这个槽位, 所以下面那几条
         * 断言与它无关; 一旦有人把折叠请求改回走组上下文, 这个夹具立刻变红。 */
        if (cJSON_GetArraySize(sys->history) == 0)
        {
            sds mainp = edc_context_build_system_prompt(h.cfg, NULL);
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "system");
            cJSON_AddStringToObject(m, "content", mainp);
            edc_task_group_history_add_owned(sys, m);
            sdsfree(mainp);
        }
        cJSON *h0 = cJSON_GetArrayItem(sys->history, 0);
        const char *c0 = h0 ? cJSON_GetStringValue(cJSON_GetObjectItem(h0, "content")) : NULL;
        log_info("  (前置: 系统组 history[0] 里躺着 = %.24s)", c0 ? c0 : "(空)");
    }

    edc_scheduler_attach_input(h.sched, fold, "SUMMARIZE-BODY");
    g_req_msgs = 0;
    edc_llm_request_send(h.sched, fold, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 1 && g_req != NULL, "折叠请求发出去了");
    if (g_req)
    {
        CHECK(cJSON_GetArraySize(g_req->history) == 1, "请求前缀只有一条消息");
        cJSON *sys0 = cJSON_GetArrayItem(g_req->history, 0);
        const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(sys0, "role"));
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(sys0, "content"));
        CHECK(role && strcmp(role, "system") == 0, "它是 role=system");
        CHECK(content && strcmp(content, edc_group_fold_system_prompt()) == 0,
              "内容逐字节 = 折叠说明 (不是主提示词)");
        CHECK(content && strstr(content, "# EdgeClaw") == NULL, "没有主提示词 (重启那条污染路径)");
        CHECK(g_req->turn == &fold->active_turn->messages, "turn 就是折叠任务的 turn (折叠 prompt)");
        CHECK(g_req->tail_system == NULL, "队尾不追加 todo 提醒 (摘要不该被插尾)");
        edc_llm_actor_req_free(g_req);
        g_req = NULL;
    }
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════ */

int main(void)
{
    test_turn_begin_honours_folded_prefix();
    test_turn_begin_zero_without_prefix();
    test_summary_request_is_capped();
    test_fold_request_is_self_contained();

    printf("\n  %d passed, %d failed\n\n", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
