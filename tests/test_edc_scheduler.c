#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_config.h"
#include "edc_channel_actor.h"
#include "edc_llm_actor.h"
#include "edc_toolbox_actor.h"
#include "edc_task_group.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
/* ── P3 ③ 测试读口 ──────────────────────────────────────────────
 * scheduler 不再持有"当前 session", 所以单测要给它装一个 store 读口: 把请求里
 * 的 session 标签解析成 session 对象 (入站路由 / 快照拉取都走它)。这一层测试
 * 没有 agent, ctx 就留给它 (agent 那层用它自己的读口)。 */
static struct edc_session *test_resolve_session(void *ctx, const char *session_id)
{
    struct edc_session *sess = ctx;
    if (!sess)
        return NULL;
    if (!session_id || !*session_id)
        return sess;
    return (sess->session_id && strcmp(session_id, sess->session_id) == 0) ? sess : NULL;
}

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    sched->resolve_session = test_resolve_session;
}

volatile sig_atomic_t _run_loop;

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while(0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        log_error("    FAIL: " __VA_ARGS__); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while(0)

/* ── 辅助: 创建最小 session/config ────────────────────────── */

static struct edc_config *make_config(void)
{
    struct edc_config *c = calloc(1, sizeof(struct edc_config));
    c->workspace = sdsnew("/tmp/test-workspace");
    return c;
}

static struct edc_session *make_session(struct edc_config *config)
{
    /* 直接分配 session 结构，跳过 edc_session_new 的初始化 */
    struct edc_session *s = calloc(1, sizeof(struct edc_session));
    s->config = config;
    return s;
}

static void free_config(struct edc_config *c)
{
    if (!c) return;
    sdsfree(c->workspace);
    free(c);
}

/* Remove the persisted task-id high-water file so a test starts from a clean
 * counter. Also make sure the workspace dir exists (writes need it). */
static void clean_task_seq(struct edc_config *cfg)
{
    if (!cfg || !cfg->workspace) return;
    mkdir(cfg->workspace, 0755); /* EEXIST is fine */
    sds seq = sdscatprintf(sdsempty(), "%s/.task_seq", cfg->workspace);
    remove(seq);
    sdsfree(seq);
}

/* P3: 入站消息经 store 读口解析 session —— 定义在下面 (drive_input 附近), 这里
 * 只是前向声明 (第一个用它的测试在上面)。 */
static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess);

/* ── 1. 生命周期 ──────────────────────────────────────────── */

static void test_scheduler_lifecycle(void)
{
    TEST("scheduler_new / free");
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    CHECK(sched != NULL, "scheduler_new returns non-NULL");
    CHECK(sched->next_task_id == 1, "next_task_id starts at 1");
    CHECK(edc_scheduler_get_actor(sched) != NULL, "get_actor returns non-NULL");
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "no foreground group initially");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("scheduler free handles NULL");
    edc_scheduler_free(NULL);
    CHECK(1, "scheduler_free(NULL) survived");
}

/* ── 2. Task 管理 ─────────────────────────────────────────── */

static void test_scheduler_task_mgmt(void)
{
    TEST("create_task generates IDs");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *t1 = edc_scheduler_create_task(sched, sess, "task one", edc_channel_adapter);
    struct edc_task *t2 = edc_scheduler_create_task(sched, sess, "task two", edc_channel_adapter);

    CHECK(t1 != NULL, "task 1 created");
    CHECK(t2 != NULL, "task 2 created");
    CHECK(t1->task_id != NULL, "task 1 id non-NULL");
    CHECK(t2->task_id != NULL, "task 2 id non-NULL");
    CHECK(strcmp(t1->task_id, t2->task_id) != 0, "task IDs are unique");
    CHECK(edc_task_is_ready(t1), "task starts READY");

    TEST("find_task_by_id cross-group");
    struct edc_task_group *g1 = edc_scheduler_create_group(sched, sess, t1);
    edc_task_group_add_task(g1, t2);

    CHECK(edc_scheduler_find_task_by_id(sched, sess, t1->task_id) == t1, "find task-1 by ID");
    CHECK(edc_scheduler_find_task_by_id(sched, sess, t2->task_id) == t2, "find task-2 by ID");
    CHECK(edc_scheduler_find_task_by_id(sched, sess, "nonexistent") == NULL, "find nonexistent → NULL");

    TEST("find_task_by_id NULL safety");
    CHECK(edc_scheduler_find_task_by_id(sched, sess, NULL) == NULL, "NULL id → NULL");
    CHECK(edc_scheduler_find_task_by_id(NULL, sess, "x") == NULL, "NULL sched → NULL");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 3. Group 管理 ────────────────────────────────────────── */

static void test_scheduler_group_mgmt(void)
{
    TEST("create_group adds to scheduler's list");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *leader = edc_scheduler_create_task(sched, sess, "group leader", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, leader);

    CHECK(g != NULL, "group created");
    CHECK(g->leader == leader, "leader set");
    CHECK(leader->is_group_leader == 1, "leader marked");
    CHECK(strcmp(g->group_id, leader->task_id) == 0, "group_id = leader task_id");

    TEST("add_group appends");
    struct edc_task *l2 = edc_scheduler_create_task(sched, sess, "group 2 leader", edc_channel_adapter);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, sess, l2);
    edc_scheduler_add_group(sched, sess, g2);
    CHECK(sess->groups == g, "first group at head");
    CHECK(sess->groups->next == g2, "second at next");

    TEST("add_group NULL safety");
    edc_scheduler_add_group(sched, sess, NULL);
    CHECK(1, "add_group(NULL) survived");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 4. 前台/后台 ─────────────────────────────────────────── */

static void test_scheduler_fg_bg(void)
{
    TEST("set_foreground / get_foreground");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *l = edc_scheduler_create_task(sched, sess, "fg leader", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, l);

    edc_scheduler_set_foreground(sess, g);
    CHECK(edc_scheduler_get_foreground(sess) == g, "foreground set correctly");

    edc_scheduler_set_foreground(sess, NULL);
    CHECK(edc_scheduler_get_foreground(sess) == NULL, "foreground cleared");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 5. 信号发送 ──────────────────────────────────────────── */

static int g_save_calls = 0;
static void fake_save_session(struct edc_scheduler *sched, struct edc_session *session)
{
    (void)sched;
    (void)session;
    g_save_calls++;
}

static void test_scheduler_signals(void)
{
    TEST("send_signal_to_task");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *t = edc_scheduler_create_task(sched, sess, "signal target", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, t);

    CHECK(edc_task_is_ready(t), "task at READY");

    edc_scheduler_send_signal_to_task(sched, sess, t->task_id, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(t), "task cancelled by SIGTERM");

    TEST("send_signal_to_task nonexistent");
    edc_scheduler_send_signal_to_task(sched, sess, "nonexistent", TASK_SIGTERM);
    CHECK(1, "signal to nonexistent task survived");

    TEST("send_signal_to_task triggers save_session on cancel");
    g_save_calls = 0;
    sched->save_session = fake_save_session;

    /* WAITING_INPUT task: cancel → must persist */
    struct edc_task *t2 = edc_scheduler_create_task(sched, sess, "signal target 2", edc_channel_adapter);
    edc_task_group_add_task(g, t2);
    edc_scheduler_send_signal_to_task(sched, sess, t2->task_id, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(t2), "t2 cancelled");
    CHECK(g_save_calls == 1, "save_session called once after explicit cancel");

    /* COMPLETED task is terminal: cancel is a no-op → no save */
    struct edc_task *t3 = edc_scheduler_create_task(sched, sess, "done task", edc_channel_adapter);
    edc_task_group_add_task(g, t3);
    edc_task_await_exec(t3);
    edc_task_mark_completed(t3);
    edc_scheduler_send_signal_to_task(sched, sess, t3->task_id, TASK_SIGTERM);
    CHECK(edc_task_is_completed(t3), "completed task stays COMPLETED");
    CHECK(g_save_calls == 1, "no extra save for no-op cancel on completed task");

    /* nonexistent task → no save */
    edc_scheduler_send_signal_to_task(sched, sess, "nonexistent", TASK_SIGTERM);
    CHECK(g_save_calls == 1, "no save for nonexistent task");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

static void test_scheduler_group_signals(void)
{
    TEST("send_signal_to_group cancels all members");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *l = edc_scheduler_create_task(sched, sess, "group L", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, l);
    struct edc_task *m1 = edc_scheduler_create_task(sched, sess, "member 1", edc_channel_adapter);
    struct edc_task *m2 = edc_scheduler_create_task(sched, sess, "member 2", edc_channel_adapter);
    edc_task_group_add_task(g, m1);
    edc_task_group_add_task(g, m2);

    edc_scheduler_send_signal_to_group(sched, sess, g->group_id, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(l), "leader cancelled");
    CHECK(edc_task_is_cancelled(m1), "member 1 cancelled");
    CHECK(edc_task_is_cancelled(m2), "member 2 cancelled");

    TEST("send_signal_to_group nonexistent");
    edc_scheduler_send_signal_to_group(sched, sess, "nonexistent-gid", TASK_SIGTERM);
    CHECK(1, "signal to nonexistent group survived");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

static void test_scheduler_signal_all(void)
{
    TEST("send_signal_to_all cancels everything");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *l1 = edc_scheduler_create_task(sched, sess, "G1L", edc_channel_adapter);
    struct edc_task_group *g1 = edc_scheduler_create_group(sched, sess, l1);
    edc_task_group_add_task(g1, edc_scheduler_create_task(sched, sess, "G1M", edc_channel_adapter));

    struct edc_task *l2 = edc_scheduler_create_task(sched, sess, "G2L", edc_channel_adapter);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, sess, l2);

    CHECK(g1->member_count == 2, "group 1 has 2 members");
    CHECK(g2->member_count == 1, "group 2 has 1 member");

    edc_scheduler_send_signal_to_all(sched, sess, TASK_SIGTERM);
    CHECK(edc_task_is_cancelled(l1), "G1 leader cancelled");
    CHECK(edc_task_is_cancelled(l2), "G2 leader cancelled");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 6. 输入路由: 普通输入一律新建 task (attach 已退休) ── */

static int g_cmd_calls = 0;
static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static bool mock_handle_command(struct edc_scheduler *sched, struct edc_actor *actor,
                                const struct edc_sched_input *input)
{
    (void)sched;
    (void)actor;
    (void)input;
    g_cmd_calls++;
    return true;
}

static void mock_send_llm(struct edc_scheduler *sched, struct edc_task *task, void *request_id)
{
    (void)sched;
    (void)request_id;
    g_llm_calls++;
    g_llm_task = task;
}

/* P3: 入站消息带一个 **session 标签**, scheduler 经 store 读口把它解析成 session
 * (它不再持有"当前 session"的副本)。单测里只有一个 session: 标签为空或与它一致
 * 就给它; 标签明确指向别人 → NULL = 调用方拒绝这条输入 (不猜)。 */

/* 驱动一条入站输入 (模拟 TUI): TUI 永远**显式**带 session 标签 (P3 ①a)。 */

/* 直接驱动 scheduler actor handler 投递一条 CHANNEL_RECV 消息 (模拟 TUI 输入)。 */
static void drive_input(struct edc_scheduler *sched, const char *content, long req_id)
{
    edc_actor_t *sched_actor = edc_scheduler_get_actor(sched);
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    /* 标签 = 该 scheduler 的读口指向的那棵树 (ctx 就是它)。 */
    struct edc_session *owner = sched->ctx;
    if (owner && owner->session_id)
        cm->session_id = sdsnew(owner->session_id);
    edc_actor_msg_t *msg =
        edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, (void *)(intptr_t)req_id, edc_channel_input_new(cm),
                          (edc_actor_data_free_fn)edc_channel_input_free);
    sched_actor->handler(sched_actor, msg); /* dispatches + frees msg */
}

static void test_input_routing(void)
{
    TEST("a parked task no longer captures plain input (attach is retired)");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    /* 旧模型里, 一个停在前台 group 的 active task 会把普通输入"吃"进去 (attach);
     * 新模型下它没有任何特殊地位 —— 输入交给命令处理器 (shell), 由它建一条新任务。 */
    struct edc_task *parked = edc_scheduler_create_task(sched, sess, "parked", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, parked);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, parked);
    CHECK(edc_task_is_ready(parked), "parked task starts READY");

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "hello", 1);

    CHECK(g_cmd_calls == 1, "plain input reached the shell (it builds the task)");
    CHECK(g_llm_calls == 0, "nothing was attached to the parked task");
    CHECK(edc_task_is_ready(parked), "parked task untouched");
    CHECK(parked->active_turn == NULL, "parked task got no turn");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("plain input spawns via shell when no parked task");
    cfg = make_config();
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    /* 已完成的前台 task → 本来就是新建 task, 现在不再有"停着就吃掉输入"那条路 */
    struct edc_task *done = edc_scheduler_create_task(sched, sess, "done", edc_channel_adapter);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, sess, done);
    edc_scheduler_set_foreground(sess, g2);
    edc_session_set_active_task(sess, done);
    edc_task_await_exec(done);
    edc_task_mark_completed(done);

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "hello", 2);

    CHECK(g_cmd_calls == 1, "shell invoked when no waiting task");
    CHECK(g_llm_calls == 0, "send_llm NOT called (shell handles spawn)");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("slash command reaches the shell");
    cfg = make_config();
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *leader2 = edc_scheduler_create_task(sched, sess, "waiting", edc_channel_adapter);
    struct edc_task_group *g3 = edc_scheduler_create_group(sched, sess, leader2);
    edc_scheduler_set_foreground(sess, g3);
    edc_session_set_active_task(sess, leader2);

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "/task list", 3);

    CHECK(g_cmd_calls == 1, "slash command reached shell");
    CHECK(g_llm_calls == 0, "slash command did not attach/dispatch anything");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── 7. max_iter: LLM 工具循环迭代预算 ────────────────────── */

static int g_dispatch_calls = 0;
static int g_reply_calls = 0;
static char g_reply_content[512];

static void mock_dispatch_tools(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                void *request_id)
{
    (void)sched;
    (void)task;
    (void)llm_resp;
    (void)request_id;
    g_dispatch_calls++;
}

static void mock_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                            edc_channel_message_type message_type, void *request_id)
{
    (void)sched;
    (void)task;
    (void)message_type;
    (void)request_id;
    g_reply_calls++;
    snprintf(g_reply_content, sizeof(g_reply_content), "%s", content ? content : "");
}

/* 投递一条 TOOL_CALL 的 LLM_RESPONSE (模拟模型又请求了一次工具)。 */
static void deliver_llm_tool_call(struct edc_scheduler *sched, long req_id)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    struct edc_llm_actor_resp *r = edc_llm_actor_resp_new();
    r->reason = BC_LLM_FINISH_REASON_TOOL_CALL;
    r->content = sdsnew("tool round");
    r->msg = cJSON_CreateObject();
    cJSON_AddStringToObject(r->msg, "role", "assistant");
    cJSON_AddStringToObject(r->msg, "content", "let me run a tool");
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_LLM_RESPONSE, NULL, (void *)(intptr_t)req_id, r,
                                             (edc_actor_data_free_fn)edc_llm_actor_resp_free);
    actor->handler(actor, msg); /* dispatches + frees msg (and resp) */
}

/* 投递一条单工具链 (tr->next == NULL) 的 TOOL_RESPONSE, 模拟工具已执行完。 */
static void deliver_tool_resp(struct edc_scheduler *sched, long req_id)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    struct edc_toolbox_actor_req *tr = calloc(1, sizeof(*tr));
    tr->name = sdsnew("exec_shell");
    tr->call_id = sdsnew("call_1");
    tr->result = sdsnew("tool result");
    tr->next = NULL;
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TOOL_RESPONSE, NULL, (void *)(intptr_t)req_id, tr, NULL);
    actor->handler(actor, msg); /* dispatches + frees msg (not data) */
    sdsfree(tr->name);
    sdsfree(tr->call_id);
    sdsfree(tr->result);
    free(tr);
}

/* 一轮工具循环: LLM 要工具 → 派发 → 工具结果回链尾。返回后由 send_llm
 * 决定是否续发 (或到 max_iter 被截断)。 */
static void run_one_tool_round(struct edc_scheduler *sched, long req_id)
{
    deliver_llm_tool_call(sched, req_id);
    deliver_tool_resp(sched, req_id);
}

static void test_scheduler_max_iter_cap(void)
{
    TEST("max_iter=2 caps tool loop after 2 rounds");
    struct edc_config *cfg = make_config();
    cfg->max_iter = 2;
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *leader = edc_scheduler_create_task(sched, sess, "leader", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, leader);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, leader);

    g_llm_calls = 0;
    g_dispatch_calls = 0;
    g_reply_calls = 0;
    g_llm_task = NULL;
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;
    sched->send_reply = mock_send_reply;

    drive_input(sched, "hello", 101);
    /* 新模型: 普通输入经命令处理器建一条**新**任务并对它派发, 不再是 attach 到那个
     * 停着的 leader。后面断言的对象换成真正拿到输入的任务 (mock_send_llm 记下了它)。 */
    leader = g_llm_task;
    CHECK(leader != NULL && leader->llm_rounds == 1, "first llm request consumes one round");
    CHECK(g_llm_calls == 1, "first llm request sent");

    run_one_tool_round(sched, 101); /* 第1轮: 链尾 rounds=1 < 2 → 续发 */
    CHECK(leader->llm_rounds == 2, "chain-end resend consumes round 2");
    CHECK(g_dispatch_calls == 1, "tools dispatched in round 1");
    CHECK(edc_task_awaits_exec(leader), "round 1 continues to llm");
    CHECK(g_llm_calls == 2, "round 1 chain end re-sends llm");

    run_one_tool_round(sched, 101); /* rounds=2 >= 2 → 截断 */
    CHECK(leader->llm_rounds == 2, "rounds consumed up to the cap");
    CHECK(g_llm_calls == 2, "no third llm request after budget exhausted");
    CHECK(edc_task_is_completed(leader), "task completed at cap");
    CHECK(leader->active_turn == NULL, "active turn committed");
    CHECK(leader->output && strstr(leader->output, "max_iter=2") != NULL, "output carries cap notice");
    CHECK(g_reply_calls == 1, "cap notice replied once");
    CHECK(strstr(g_reply_content, "max_iter=2") != NULL, "reply content explains budget");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("max_iter=1 stops immediately after first tool chain");
    cfg = make_config();
    cfg->max_iter = 1;
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    leader = edc_scheduler_create_task(sched, sess, "leader", edc_channel_adapter);
    g = edc_scheduler_create_group(sched, sess, leader);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, leader);

    g_llm_calls = 0;
    g_reply_calls = 0;
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;
    sched->send_reply = mock_send_reply;

    drive_input(sched, "hello", 102);
    leader = g_llm_task; /* 输入建的新任务 (见上) */
    run_one_tool_round(sched, 102);
    CHECK(g_llm_calls == 1, "only the initial llm request");
    CHECK(edc_task_is_completed(leader), "capped right after first chain");
    CHECK(g_reply_calls == 1, "cap notice replied");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("max_iter<=0 keeps unlimited loop (legacy behavior)");
    cfg = make_config(); /* calloc → max_iter == 0 */
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    leader = edc_scheduler_create_task(sched, sess, "leader", edc_channel_adapter);
    g = edc_scheduler_create_group(sched, sess, leader);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, leader);

    g_llm_calls = 0;
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;
    sched->send_reply = mock_send_reply;

    drive_input(sched, "hello", 103);
    leader = g_llm_task; /* 输入建的新任务 (见上) */
    run_one_tool_round(sched, 103);
    run_one_tool_round(sched, 103);
    run_one_tool_round(sched, 103);
    CHECK(leader->llm_rounds == 4, "rounds counted while unlimited");
    CHECK(g_llm_calls == 4, "llm keeps being re-sent (no cap)");
    CHECK(leader->active_turn != NULL, "turn stays open while looping");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);

    TEST("max_iter=-1 keeps unlimited loop (default sentinel)");
    cfg = make_config();
    cfg->max_iter = -1;
    sess = make_session(cfg);
    sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    leader = edc_scheduler_create_task(sched, sess, "leader", edc_channel_adapter);
    g = edc_scheduler_create_group(sched, sess, leader);
    edc_scheduler_set_foreground(sess, g);
    edc_session_set_active_task(sess, leader);

    g_llm_calls = 0;
    sched->send_llm = mock_send_llm;
    sched->dispatch_tools = mock_dispatch_tools;
    sched->send_reply = mock_send_reply;

    drive_input(sched, "hello", 104);
    leader = g_llm_task; /* 输入建的新任务 (见上) */
    run_one_tool_round(sched, 104);
    run_one_tool_round(sched, 104);
    CHECK(leader->llm_rounds == 3, "rounds counted while unlimited (-1)");
    CHECK(g_llm_calls == 3, "llm keeps being re-sent with -1 (no cap)");
    CHECK(leader->active_turn != NULL, "turn stays open while looping (-1)");

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── main ──────────────────────────────────────────────────── */

/* ── 任务树快照: 只带侧边栏数据, 不再携带对话消息 ───────────── */

static sds g_snapshot_json = NULL;

static void capture_snapshot(struct edc_scheduler *sched, sds serialized)
{
    (void)sched;
    if (g_snapshot_json)
        sdsfree(g_snapshot_json);
    g_snapshot_json = serialized; /* 接管所有权 */
}

static cJSON *snap_mk_msg(const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    return m;
}

/* 遍历快照 groups[], 取 fg 组。 */
static cJSON *snap_find_fg(cJSON *root)
{
    cJSON *groups = root ? cJSON_GetObjectItem(root, "groups") : NULL;
    if (!cJSON_IsArray(groups))
        return NULL;
    cJSON *gj;
    cJSON_ArrayForEach(gj, groups)
    {
        if (cJSON_IsTrue(cJSON_GetObjectItem(gj, "fg")))
            return gj;
    }
    return NULL;
}

/* P3 ①b: 快照是**拉取**的 —— 请求里点名要哪棵树 (data = session_id)。 */
static void drive_snapshot(struct edc_scheduler *sched, struct edc_session *session)
{
    edc_actor_t *actor = edc_scheduler_get_actor(sched);
    edc_actor_msg_t *msg = edc_actor_msg_new_sub_type(BC_ACTOR_MSG_TASK_SNAPSHOT, 0, NULL, NULL,
                                                      sdsnew(session ? session->session_id : ""),
                                                      (edc_actor_data_free_fn)sdsfree);
    actor->handler(actor, msg); /* dispatches + frees msg */
}

static void test_snapshot_carries_no_chat_messages(void)
{
    TEST("task snapshot carries sidebar data, never chat messages");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, sess);

    struct edc_task *leader = edc_scheduler_create_task(sched, sess, "leader", edc_channel_adapter);
    struct edc_task *member = edc_scheduler_create_task(sched, sess, "member", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, leader);
    edc_task_group_add_task(g, member);
    edc_scheduler_set_foreground(sess, g);

    /* 组历史里有消息, 但快照不应把对话内容下发 —— 聊天区由 TUI 自己持久化。 */
    edc_task_group_history_add_owned(g, snap_mk_msg("system", "group prompt"));
    edc_task_group_history_add_owned(g, snap_mk_msg("user", "q"));

    g_snapshot_json = NULL;
    sched->snapshot_reply = capture_snapshot;
    drive_snapshot(sched, sess);

    cJSON *root = g_snapshot_json ? cJSON_Parse(g_snapshot_json) : NULL;
    CHECK(root != NULL, "snapshot parses as JSON");

    cJSON *fg = snap_find_fg(root);
    CHECK(fg != NULL, "foreground group present");
    CHECK(fg && cJSON_GetObjectItem(fg, "messages") == NULL, "snapshot has no messages[]");
    CHECK(fg && cJSON_GetArraySize(cJSON_GetObjectItem(fg, "tasks")) == 2, "both tasks present");
    cJSON_Delete(root);

    if (g_snapshot_json)
        sdsfree(g_snapshot_json);
    g_snapshot_json = NULL;

    edc_scheduler_free(sched);
    free(sess);
    free_config(cfg);
}

/* ── Task 序号跨重启持久化 ────────────────────────────────── */

static void test_task_id_survives_restart(void)
{
    TEST("task_id does not restart from 1 after a restart");
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);

    /* 第一次运行: 连发 3 个编号, 随后把它们全部回收 (/task join 的典型路径:
     * 编号已发出但任务对象不再存活), 再"退出"。 */
    {
        struct edc_session *sess = make_session(cfg);
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        struct edc_task *t1 = edc_scheduler_create_task(sched, sess, "a", edc_channel_adapter);
        struct edc_task *t2 = edc_scheduler_create_task(sched, sess, "b", edc_channel_adapter);
        struct edc_task *t3 = edc_scheduler_create_task(sched, sess, "c", edc_channel_adapter);
        CHECK(t1 && strcmp(t1->task_id, "1") == 0, "first id is 1");
        CHECK(t3 && strcmp(t3->task_id, "3") == 0, "third id is 3");
        edc_task_free(t1);
        edc_task_free(t2);
        edc_task_free(t3);
        edc_scheduler_free(sched);
        free(sess);
    }

    /* 重启: 存活任务为空 (全部被回收), 发号器仍必须从 4 续, 不能退回 1。 */
    {
        struct edc_session *sess = make_session(cfg);
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        struct edc_task *t = edc_scheduler_create_task(sched, sess, "d", edc_channel_adapter);
        CHECK(t && strcmp(t->task_id, "4") == 0, "id continues at 4 after restart (no reuse)");
        edc_task_free(t);
        edc_scheduler_free(sched);
        free(sess);
    }

    /* 收尾: 清掉高水位文件, 免得污染其它测试 / 重复运行。 */
    clean_task_seq(cfg);
    free_config(cfg);
}

/* 反序列化旧存档时, 存活任务的编号要 "喂" 进发号器 (并落盘高水位), 否则
 * 从没有 .task_seq 的旧版本升级上来会重复发号。 */
static void test_observe_task_id_raises_high_water(void)
{
    TEST("observe_task_id raises and persists the high-water mark");
    struct edc_config *cfg = make_config();
    clean_task_seq(cfg);

    {
        struct edc_session *sess = make_session(cfg);
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        CHECK(sched->next_task_id == 1, "fresh counter is 1");
        edc_scheduler_observe_task_id(sched, "7");
        CHECK(sched->next_task_id == 8, "observed id 7 raises counter to 8");
        edc_scheduler_observe_task_id(sched, "task-3"); /* non-numeric: ignored */
        CHECK(sched->next_task_id == 8, "non-numeric id is ignored");
        edc_scheduler_free(sched);
        free(sess);
    }

    /* 重启: 高水位从磁盘恢复。 */
    {
        struct edc_session *sess = make_session(cfg);
        struct edc_scheduler *sched = edc_scheduler_new(cfg);
        install_resolver(sched, sess);
        CHECK(sched->next_task_id == 8, "high-water survives restart");
        char *id = edc_scheduler_gen_task_id(sched);
        CHECK(strcmp(id, "8") == 0, "next generated id is 8");
        free(id);
        edc_scheduler_free(sched);
        free(sess);
    }

    clean_task_seq(cfg);
    free_config(cfg);
}

int main(void)
{
    log_info("=== Phase 3: edc_scheduler 模块测试 ===");
    log_info("");

    test_scheduler_lifecycle();
    test_scheduler_task_mgmt();
    test_scheduler_group_mgmt();
    test_scheduler_fg_bg();
    test_scheduler_signals();
    test_scheduler_group_signals();
    test_scheduler_signal_all();
    test_input_routing();
    test_scheduler_max_iter_cap();
    test_snapshot_carries_no_chat_messages();
    test_task_id_survives_restart();
    test_observe_task_id_raises_high_water();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
