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
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_context.h"
#include "edc_llm_actor.h"
#include "edc_group_fold.h"
#include "edc_llm_request.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_memory.h"
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
    h->cfg->workspace = sdsnew(edc_test_workspace());
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
    struct edc_task *leader = edc_task_new(edc_test_id("1"), sid, edc_test_id("1"), "leader", edc_channel_adapter);
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
    CHECK(turn->msg_count == 3, "当前 turn 有 3 条");

    /* 折叠切在这个 turn 内部: 前缀 1 条进了组记忆。 */
    turn->folded_prefix = 1;
    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
    pump(&h);

    CHECK(g_req_msgs == 1, "请求真发出去了");

    if (g_req)
    {
        /* The request carries the tail span it loaded (folded prefix already excluded):
         * the turn has 3 messages and folded_prefix is 1, so 2 go on the wire. */
        CHECK(cJSON_GetArraySize(g_req->turn_tail) == 2,
              "turn_tail 只含折叠前缀之后的消息 (前缀不再重复发送)");
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

    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
    pump(&h);

    CHECK(g_req_msgs == 1, "请求发出去了");
    if (g_req)
        CHECK(cJSON_GetArraySize(g_req->turn_tail) == turn->msg_count,
              "turn_tail 是整条 turn (没有折叠前缀)");

    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 2b: L2 session 记忆通知 (P4c) —— 请求期临时物, 队尾第一条
 *  ══════════════════════════════════════════════════════════════ */

/* 请求的 tail_parts 里那条通知 (没有就 NULL)。 */
static const char *notice_part(struct edc_llm_actor_req *req, int *out_index)
{
    if (out_index)
        *out_index = -1;
    if (!req || !req->tail_parts)
        return NULL;
    int n = cJSON_GetArraySize(req->tail_parts);
    for (int i = 0; i < n; i++)
    {
        cJSON *m = cJSON_GetArrayItem(req->tail_parts, i);
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
        if (content && strncmp(content, "## Session Memory Updates", 25) == 0)
        {
            if (out_index)
                *out_index = i;
            return content;
        }
    }
    return NULL;
}

static sds memo_add(struct edc_session *sess, const char *title, const char *summary, const char *detail)
{
    sds id = edc_session_memory_create_entry(sess->memo, title, summary, detail, NULL);
    uint64_t seq = edc_session_memory_add_event(sess->memo, id, EDC_MEMO_CREATED, title, summary);
    struct edc_memo_entry *e = edc_session_memory_entry(sess->memo, id);
    if (e)
        e->created_seq = e->updated_seq = seq;
    return id;
}

static void test_session_memory_notice_reaches_the_tail(void)
{
    TEST("L2 通知: 新 task 首轮自带回填; 队尾第一条; 提交前不发第二次; 提交后为空");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-req");
    (void)g;
    attach_turn(&h, leader, "current input");

    /* 两条条目 (与前台 memory_write 走同一套) + 一个还没有东西的 task */
    sds id1 = memo_add(h.sess, "构建入口", "唯一入口是 bazelisk", "明细一");
    sds id2 = memo_add(h.sess, "沙箱策略", "uid 1000 + sudo", NULL);
    leader->memo_seen_seq = 0; /* 新 task: 什么都没看过 ⇒ 该回填 */

    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
    pump(&h);

    CHECK(g_req_msgs == 1, "请求发出去了");
    int idx = -1;
    const char *notice = notice_part(g_req, &idx);
    CHECK(notice != NULL, "队尾有 session 记忆通知");
    CHECK(notice && strstr(notice, "[m1] created: 构建入口 — 唯一入口是 bazelisk") != NULL, "第一条在");
    CHECK(notice && strstr(notice, "[m2] created: 沙箱策略 — uid 1000 + sudo") != NULL, "第二条在");
    CHECK(notice && strstr(notice, "共 2 条") != NULL, "带总条数");
    CHECK(idx == 0, "通知在队尾**第一条** (todo 提醒在它后面)");
    CHECK(leader->memo_seen_seq == 0, "水位没动 —— 请求还没被模型读到");
    CHECK(leader->memo_notice_seq == 2, "记下了这次装进去的最大 seq");

    /* 还没提交 (等价于请求被取消): 下一次请求**重发同一条** */
    edc_llm_actor_req_free(g_req);
    g_req = NULL;
    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 2, "第二次请求也发出去了");
    CHECK(notice_part(g_req, NULL) != NULL, "没提交 ⇒ 通知重发 (幂等, 不丢信息)");

    /* 响应回来 ⇒ 提交 ⇒ 再请求就不带了 */
    edc_task_memo_commit(leader);
    CHECK(leader->memo_seen_seq == 2, "提交后水位追平");
    edc_llm_actor_req_free(g_req);
    g_req = NULL;
    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 3, "第三次请求");
    CHECK(notice_part(g_req, NULL) == NULL, "看过之后不再注入 (常驻里也没有 memory 内容)");

    /* 新条目到来 ⇒ 只补新的那条 */
    sds id3 = memo_add(h.sess, "第三条", "后来才有的", NULL);
    edc_llm_actor_req_free(g_req);
    g_req = NULL;
    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    const char *n2 = notice_part(g_req, NULL);
    CHECK(n2 != NULL && strstr(n2, id3) != NULL, "只带新条目");
    CHECK(n2 && strstr(n2, "共 3 条") != NULL, "总数跟着涨");
    edc_llm_actor_req_free(g_req);
    g_req = NULL;

    /* 折叠任务 (sink = GROUP_MEMORY): 自足请求, 队尾什么都不加 */
    struct edc_task *fold = edc_scheduler_spawn_system_task(h.sched, h.sess, "fold group memory");
    attach_turn(&h, fold, "fold prompt material");
    fold->sink = EDC_SINK_GROUP_MEMORY;
    edc_llm_request_send(h.sched, fold, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(notice_part(g_req, NULL) == NULL, "折叠任务不吃通知 (请求自足)");

    sdsfree(id1);
    sdsfree(id2);
    sdsfree(id3);
    harness_down(&h);
}

/* 头部两块的顺序与"同组内逐字节一致" —— cache 命中的前提：
 * 常量 (system prompt) 在前、易变的 (group memory) 在后；同组内不同 task 的头部两块必须完全
 * 相同, 否则它们连第一段 cache 都共享不了。 */
static void test_head_blocks_cache_layout(void)
{
    TEST("请求头部: system prompt 在前 / group memory 在后; 同组两个 task 逐字节一致");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-head");
    g->memory = sdsnew("MEM-SUMMARY"); /* 非空 ⇒ 两块都出现 */

    struct edc_task *t2 = edc_task_new(edc_test_id("h-2"), "sess-head", g->group_id, "second task", edc_channel_adapter);
    edc_task_group_add_task(g, t2);

    attach_turn(&h, leader, "first input");
    attach_turn(&h, t2, "second input");

    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 1, "A 的请求发出去了");

    sds a0 = NULL, a1 = NULL;
    if (g_req && g_req->history)
    {
        a0 = sdsnew(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g_req->history, 0), "content")));
        a1 = sdsnew(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g_req->history, 1), "content")));
        CHECK(a0 && strstr(a0, "# EdgeClaw") != NULL, "第 0 块 = 系统提示词 (常量)");
        CHECK(a1 && strncmp(a1, "## Group Memory\nMEM-SUMMARY", 27) == 0, "第 1 块 = group memory (易变的在后)");
    }
    edc_llm_actor_req_free(g_req);
    g_req = NULL;
    g_req_msgs = 0;

    edc_llm_request_send(h.sched, t2, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 1, "B 的请求发出去了");
    if (g_req && g_req->history)
    {
        const char *b0 = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g_req->history, 0), "content"));
        const char *b1 = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g_req->history, 1), "content"));
        CHECK(a0 && b0 && strcmp(b0, a0) == 0, "系统提示词那块与 A 逐字节一致");
        CHECK(a1 && b1 && strcmp(b1, a1) == 0, "group memory 那块也与 A 逐字节一致");
    }
    sdsfree(a0);
    sdsfree(a1);
    edc_llm_actor_req_free(g_req);
    g_req = NULL;
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 2d: JOIN 注入的形状 (P4d) —— 记忆型源只注入 output
 *  ══════════════════════════════════════════════════════════════ */

static void test_join_injection_shape(void)
{
    TEST("JOIN (B 方案): 材料拼进目标**自己的输入**; 不往请求里插消息; 只拼一次且留存");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-inj");
    (void)g;

    /* 源一: 普通任务 (description + output) */
    struct edc_task *src = edc_task_new(edc_test_id("src-1"), "sess-inj", edc_test_id("src-1"), "src work", edc_channel_adapter);
    {
        struct edc_turn *t = edc_turn_new();
        t->task_id = edc_test_id("src-1");
        cJSON *um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        cJSON_AddStringToObject(um, "content", "SOURCE-USER-TEXT");
        edc_turn_add_message(t, um);
        edc_task_add_turn(src, t);
    }
    src->output = sdsnew("SOURCE-OUTPUT-TEXT");
    edc_task_await_exec(src);
    edc_task_mark_completed(src);

    /* 源二: 记忆型 (折叠任务) —— 它的 prompt 不该被拼进来, 只拼 output */
    struct edc_task *fold_src = edc_scheduler_spawn_system_task(h.sched, h.sess, "fold group memory");
    {
        struct edc_turn *t = edc_turn_new();
        t->task_id = fold_src->task_id;
        cJSON *um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        cJSON_AddStringToObject(um, "content", "FOLD-PROMPT-TEXT");
        edc_turn_add_message(t, um);
        edc_task_add_turn(fold_src, t);
    }
    fold_src->output = sdsnew("FOLD-OUTPUT-MEMORY");
    edc_task_await_exec(fold_src);
    edc_task_mark_completed(fold_src);

    /* 目标: 自己的输入 = "DEST-OWN-TURN", 同时 JOIN 两个源 */
    struct edc_task *dst = edc_task_new(edc_test_id("dst-1"), "sess-inj", edc_test_id("dst-1"), "dst work", edc_channel_adapter);
    edc_task_add_dependency(dst, src, TASK_DEP_JOIN);
    edc_task_add_dependency(dst, fold_src, TASK_DEP_JOIN);
    attach_turn(&h, dst, "DEST-OWN-TURN");

    g_req_msgs = 0;
    edc_llm_request_send(h.sched, dst, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 1, "请求发出去了");

    /* 目标自己的输入被就地改写: 原文 + 两段材料 */
    const char *input = NULL;
    if (dst->active_turn && dst->active_turn->msg_count > 0)
    {
        cJSON *m = edc_turn_msg_at(dst->active_turn, 0);
        input = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
    }
    CHECK(input && strstr(input, "DEST-OWN-TURN") != NULL, "原输入还在");
    CHECK(input && strstr(input, "[subtask src work result]:") != NULL, "普通源: 带标记拼进输入");
    CHECK(input && strstr(input, "SOURCE-OUTPUT-TEXT") != NULL, "普通源: output 在输入里");
    CHECK(input && strstr(input, "SOURCE-USER-TEXT") == NULL, "普通源: 不再注入它那条 user 消息 (B 方案)");
    CHECK(input && strstr(input, "FOLD-OUTPUT-MEMORY") != NULL, "记忆型源: output 也拼进输入");
    CHECK(input && strstr(input, "FOLD-PROMPT-TEXT") == NULL, "记忆型源: 它的 prompt 一字不进");

    /* 请求头部里**不该**多出注入消息 (只剩系统提示词) */
    CHECK(g_req && g_req->history && cJSON_GetArraySize(g_req->history) == 1,
          "请求头部没有插入额外的注入消息 (材料走输入)");
    edc_llm_actor_req_free(g_req);
    g_req = NULL;
    g_req_msgs = 0;

    /* 再发一次: 材料已进输入 ⇒ 不重复拼 (一次性), 且仍在那里 (留存) */
    edc_llm_request_send(h.sched, dst, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 1, "第二次请求也出去了");
    const char *input2 = NULL;
    if (dst->active_turn && dst->active_turn->msg_count > 0)
    {
        cJSON *m = edc_turn_msg_at(dst->active_turn, 0);
        input2 = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
    }
    int marker_hits = 0;
    if (input2)
    {
        for (const char *q = input2; (q = strstr(q, "[subtask src work result]:")) != NULL; q += 1)
            marker_hits++;
    }
    CHECK(marker_hits == 1, "材料只在输入里出现一次 (不重复拼)");
    CHECK(input2 && strstr(input2, "SOURCE-OUTPUT-TEXT") != NULL, "第二轮的输入里材料还在 (留存)");
    CHECK(g_req && g_req->history && cJSON_GetArraySize(g_req->history) == 1, "第二轮头部依然只有系统提示词");
    edc_llm_actor_req_free(g_req);
    g_req = NULL;
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════
 *  Case 3: 摘要调用的 max_tokens (P2b-2)
 *  ══════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════
 *  Case 2c: L2 策展请求也是自足的 (P4d)
 *  ══════════════════════════════════════════════════════════════ */

static void test_curate_request_is_self_contained(void)
{
    TEST("策展请求: 只有策展说明 + 它自己的 turn; 不吃通知 / 不吃 todo");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-curate");
    (void)g;
    /* 会话里放两条条目: 策展任务也不该被"通知" (它读的是自己 turn 里的材料) */
    sds id1 = memo_add(h.sess, "构建入口", "bazelisk", NULL);
    sds id2 = memo_add(h.sess, "沙箱", "uid 1000", NULL);

    struct edc_task *l2 = edc_scheduler_spawn_system_task_as(h.sched, h.sess, "curate session memory",
                                                             EDC_SINK_SESSION_MEMORY);
    CHECK(l2 != NULL && l2->sink == EDC_SINK_SESSION_MEMORY, "起一个策展任务");
    edc_scheduler_attach_input(h.sched, l2, "CURATE-MATERIAL");
    /* 记忆工具子集: 策展任务只拿到它, 不是全量工具定义 */
    cJSON *mem_tools = cJSON_CreateArray();
    cJSON_AddItemToArray(mem_tools, cJSON_CreateObject());
    g_req_msgs = 0;
    edc_llm_request_send(h.sched, l2, NULL, mem_tools, h.llm_actor, NULL);
    pump(&h);

    CHECK(g_req_msgs == 1, "请求发出去了");
    if (g_req)
    {
        CHECK(cJSON_GetArraySize(g_req->history) == 1, "前缀只有一条消息");
        cJSON *sys0 = cJSON_GetArrayItem(g_req->history, 0);
        const char *content = sys0 ? cJSON_GetStringValue(cJSON_GetObjectItem(sys0, "content")) : NULL;
        CHECK(content && strcmp(content, edc_session_memory_system_prompt()) == 0, "内容 = 策展说明");
        CHECK(content && strstr(content, "# EdgeClaw") == NULL, "没有主提示词");
        CHECK(g_req->tools == mem_tools, "带上了记忆工具子集 (不是全量)");
        CHECK(g_req->tail_parts == NULL, "队尾什么都不加 (通知与 todo 都不给策展调用)");
        CHECK(g_req->max_tokens > 0, "带上 max_tokens (摘要类调用的唯一例外)");
        edc_llm_actor_req_free(g_req);
        g_req = NULL;
    }
    cJSON_Delete(mem_tools);

    sdsfree(id1);
    sdsfree(id2);
    harness_down(&h);
}

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
    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
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
    edc_llm_request_send(h.sched, fold, NULL, NULL, h.llm_actor, NULL);
    pump(&h);
    CHECK(g_req_msgs == 2 && g_req != NULL, "摘要请求发出去了");
    if (g_req)
    {
        CHECK(g_req->max_tokens == EDC_FOLD_SUMMARY_MAX_TOKENS, "摘要请求带 max_tokens = 摘要预算");
        CHECK(g_req->max_tokens == 32768,
              "就是 32768 (旧值 0.8 x reserve = 13107 会让长语料必然截断 —— 见 edc_group_fold.h 的 a)");
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
        /* The history is derived: dropping it is what a restart leaves behind. */
        edc_task_group_history_reset(sys);
        edc_task_group_rebuild_history(sys);
        /* 显式摆出**修复前重启后**的真形状: 那个槽位里躺着主提示词 (当时的加载期
         * 就是这么填的, 实测确认过)。修复后折叠请求不读这个槽位, 所以下面那几条
         * 断言与它无关; 一旦有人把折叠请求改回走组上下文, 这个夹具立刻变红。 */
        if (edc_task_group_history_count(sys) == 0)
        {
            sds mainp = edc_context_build_system_prompt(h.cfg, NULL);
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "system");
            cJSON_AddStringToObject(m, "content", mainp);
            edc_task_group_history_add_owned(sys, m);
            sdsfree(mainp);
        }
        cJSON *h0 = edc_task_group_history_at(sys, 0);
        const char *c0 = h0 ? cJSON_GetStringValue(cJSON_GetObjectItem(h0, "content")) : NULL;
        log_info("  (前置: 系统组 history[0] 里躺着 = %.24s)", c0 ? c0 : "(空)");
        if (h0)
            cJSON_Delete(h0);
    }

    edc_scheduler_attach_input(h.sched, fold, "SUMMARIZE-BODY");
    g_req_msgs = 0;
    edc_llm_request_send(h.sched, fold, NULL, NULL, h.llm_actor, NULL);
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
        CHECK(g_req->turn_tail != NULL, "turn_tail 就是折叠任务的 turn (折叠 prompt)");
        CHECK(g_req->tail_parts == NULL, "队尾什么都不追加 (摘要不该被插尾)");
        edc_llm_actor_req_free(g_req);
        g_req = NULL;
    }
    harness_down(&h);
}

/* ══════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════
 *  `!cmd`: the tool-only turn must reach the NEXT request
 *
 *  A `!cmd` task is one "user asked / exec_shell ran / here is the output" turn. Its
 *  whole reason to exist as a task is that this turn enters the group's history, so the
 *  next request carries the command AND its output. The three messages are built by the
 *  same calls the production path uses (edc_scheduler_task_adopt seeds user +
 *  assistant(tool_calls); tool_done appends the tool message;
 *  exec_tool_only_finish hands the turn to the group history), so what this pins is the
 *  assembly, not a re-implementation. ══════════════════════════════════════════════ */

static void test_tool_only_turn_reaches_the_next_request(void)
{
    TEST("`!cmd`: 命令 + 输出 (user / assistant(tool_calls) / tool) 原样进下一轮请求");

    struct harness h;
    harness_up(&h);
    struct edc_task *leader = NULL;
    struct edc_task_group *g = make_group(&h, &leader, "sess-req");

    /* The completed `!cmd` task's turn. The key is bound at CREATION: messages are rows
     * written under it, so assigning task_id afterwards would leave them under the
     * constructor's placeholder and history_add_turn would find nothing. */
    struct edc_turn *done = edc_turn_new_for("sess-req", edc_test_id("2"), edc_test_id("2"), 0);
    edc_turn_add_user_message(done, "!pwd");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "command", "pwd");
    edc_turn_add_assistant_tool_call(done, "call_local_2", "exec_shell", args);
    cJSON_Delete(args);
    edc_turn_add_tool_message(done, "call_local_2", "exec_shell", "/home/conley/workspace\n");
    edc_task_group_history_add_turn(g, done);

    /* The next round: a fresh turn on the leader. */
    attach_turn(&h, leader, "next question");
    edc_llm_request_send(h.sched, leader, NULL, NULL, h.llm_actor, NULL);
    pump(&h);

    CHECK(g_req_msgs == 1, "请求真发出去了");
    char order[4] = {0};
    int n = 0;
    bool cmd_seen = false, output_seen = false;
    if (g_req && g_req->history)
    {
        for (int i = 0; i < cJSON_GetArraySize(g_req->history) && n < 3; i++)
        {
            cJSON *m = cJSON_GetArrayItem(g_req->history, i);
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
            if (!role)
                continue;
            if (strcmp(role, "user") == 0)
            {
                const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
                if (c && strcmp(c, "!pwd") == 0)
                {
                    order[n++] = 'u';
                    cmd_seen = true;
                }
            }
            else if (strcmp(role, "assistant") == 0)
            {
                cJSON *tcs = cJSON_GetObjectItem(m, "tool_calls");
                const char *id = tcs ? cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(tcs, 0), "id")) : NULL;
                if (id && strcmp(id, "call_local_2") == 0)
                    order[n++] = 'a';
            }
            else if (strcmp(role, "tool") == 0)
            {
                const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "tool_call_id"));
                const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
                if (id && strcmp(id, "call_local_2") == 0)
                {
                    order[n++] = 't';
                    output_seen = c && strstr(c, "/home/conley/workspace") != NULL;
                }
            }
        }
    }

    CHECK(n == 3, "三条都在 (user + assistant(tool_calls) + tool)");
    CHECK(strcmp(order, "uat") == 0, "顺序是 user -> assistant(tool_calls) -> tool (>每一条 tool 都有配对的 assistant)");
    CHECK(cmd_seen && output_seen, "命令和它的输出都在请求里");

    harness_down(&h);
}

/* 存进 turn 的消息会挂一些"只给我们自己看"的键（折叠粘贴的 payload、工具集变更的
 * tools_added / tools_removed / toolset_reason / pinned）。它们**不能**上 wire：OpenAI 对
 * message 上的未知字段是整单 400。这条测试钉住的是白名单策略本身 —— 漏一个键，请求就打挂。 */
static void test_wire_message_keys_are_allowlisted(void)
{
    TEST("wire: message 只带 provider 认识的键（store-only 一律剥掉）");

    const char *allowed[] = {"role", "content", "name", "tool_call_id", "tool_calls", "function_call", "refusal"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        CHECK(edc_llm_wire_key_is_standard(allowed[i]), "wire key allowed");

    const char *dropped[] = {"payload", "tools_added", "tools_removed", "toolset_reason", "pinned"};
    for (size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]); i++)
        CHECK(!edc_llm_wire_key_is_standard(dropped[i]), "store-only key dropped");
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    test_wire_message_keys_are_allowlisted();
    test_turn_begin_honours_folded_prefix();
    test_turn_begin_zero_without_prefix();
    test_session_memory_notice_reaches_the_tail();
    test_head_blocks_cache_layout();
    test_join_injection_shape();
    test_curate_request_is_self_contained();
    test_summary_request_is_capped();
    test_fold_request_is_self_contained();
    test_tool_only_turn_reaches_the_next_request();

    printf("\n  %d passed, %d failed\n\n", test_passed, test_failed);
    return test_failed ? 1 : 0;
}
