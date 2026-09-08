#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_config.h"
#include "edc_channel_actor.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

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
    s->messages = cJSON_CreateArray();
    return s;
}

static void free_config(struct edc_config *c)
{
    if (!c) return;
    sdsfree(c->workspace);
    free(c);
}

/* ── 1. 生命周期 ──────────────────────────────────────────── */

static void test_scheduler_lifecycle(void)
{
    TEST("scheduler_new / free");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(sess, cfg);

    CHECK(sched != NULL, "scheduler_new returns non-NULL");
    CHECK(sched->next_task_id == 1, "next_task_id starts at 1");
    CHECK(edc_scheduler_get_actor(sched) != NULL, "get_actor returns non-NULL");
    CHECK(edc_scheduler_get_foreground(sched) == NULL, "no foreground group initially");

    edc_scheduler_free(sched);
    if (sess->messages) cJSON_Delete(sess->messages);
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
    struct edc_scheduler *sched = edc_scheduler_new(sess, cfg);

    struct edc_task *t1 = edc_scheduler_create_task(sched, "task one", edc_channel_cli);
    struct edc_task *t2 = edc_scheduler_create_task(sched, "task two", edc_channel_cli);

    CHECK(t1 != NULL, "task 1 created");
    CHECK(t2 != NULL, "task 2 created");
    CHECK(t1->task_id != NULL, "task 1 id non-NULL");
    CHECK(t2->task_id != NULL, "task 2 id non-NULL");
    CHECK(strcmp(t1->task_id, t2->task_id) != 0, "task IDs are unique");
    CHECK(t1->state == TASK_WAITING_INPUT, "task starts WAITING_INPUT");

    TEST("find_task_by_id cross-group");
    struct edc_task_group *g1 = edc_scheduler_create_group(sched, t1);
    edc_task_group_add_task(g1, t2);

    CHECK(edc_scheduler_find_task_by_id(sched, t1->task_id) == t1, "find task-1 by ID");
    CHECK(edc_scheduler_find_task_by_id(sched, t2->task_id) == t2, "find task-2 by ID");
    CHECK(edc_scheduler_find_task_by_id(sched, "nonexistent") == NULL, "find nonexistent → NULL");

    TEST("find_task_by_id NULL safety");
    CHECK(edc_scheduler_find_task_by_id(sched, NULL) == NULL, "NULL id → NULL");
    CHECK(edc_scheduler_find_task_by_id(NULL, "x") == NULL, "NULL sched → NULL");

    edc_scheduler_free(sched);
    if (sess->messages) cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);
}

/* ── 3. Group 管理 ────────────────────────────────────────── */

static void test_scheduler_group_mgmt(void)
{
    TEST("create_group adds to scheduler's list");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(sess, cfg);

    struct edc_task *leader = edc_scheduler_create_task(sched, "group leader", edc_channel_cli);
    struct edc_task_group *g = edc_scheduler_create_group(sched, leader);

    CHECK(g != NULL, "group created");
    CHECK(g->leader == leader, "leader set");
    CHECK(leader->is_group_leader == 1, "leader marked");
    CHECK(strcmp(g->group_id, leader->task_id) == 0, "group_id = leader task_id");

    TEST("add_group appends");
    struct edc_task *l2 = edc_scheduler_create_task(sched, "group 2 leader", edc_channel_cli);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, l2);
    edc_scheduler_add_group(sched, g2);
    CHECK(sched->session->groups == g, "first group at head");
    CHECK(sched->session->groups->next == g2, "second at next");

    TEST("add_group NULL safety");
    edc_scheduler_add_group(sched, NULL);
    CHECK(1, "add_group(NULL) survived");

    edc_scheduler_free(sched);
    if (sess->messages) cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);
}

/* ── 4. 前台/后台 ─────────────────────────────────────────── */

static void test_scheduler_fg_bg(void)
{
    TEST("set_foreground / get_foreground");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(sess, cfg);

    struct edc_task *l = edc_scheduler_create_task(sched, "fg leader", edc_channel_cli);
    struct edc_task_group *g = edc_scheduler_create_group(sched, l);

    edc_scheduler_set_foreground(sched, g);
    CHECK(edc_scheduler_get_foreground(sched) == g, "foreground set correctly");

    edc_scheduler_set_foreground(sched, NULL);
    CHECK(edc_scheduler_get_foreground(sched) == NULL, "foreground cleared");

    edc_scheduler_free(sched);
    if (sess->messages) cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);
}

/* ── 5. 信号发送 ──────────────────────────────────────────── */

static void test_scheduler_signals(void)
{
    TEST("send_signal_to_task");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(sess, cfg);

    struct edc_task *t = edc_scheduler_create_task(sched, "signal target", edc_channel_cli);
    struct edc_task_group *g = edc_scheduler_create_group(sched, t);

    CHECK(t->state == TASK_WAITING_INPUT, "task at WAITING_INPUT");

    edc_scheduler_send_signal_to_task(sched, t->task_id, TASK_SIGTERM);
    CHECK(t->state == TASK_CANCELLED, "task cancelled by SIGTERM");

    TEST("send_signal_to_task nonexistent");
    edc_scheduler_send_signal_to_task(sched, "nonexistent", TASK_SIGTERM);
    CHECK(1, "signal to nonexistent task survived");

    edc_scheduler_free(sched);
    if (sess->messages) cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);
}

static void test_scheduler_group_signals(void)
{
    TEST("send_signal_to_group cancels all members");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(sess, cfg);

    struct edc_task *l = edc_scheduler_create_task(sched, "group L", edc_channel_cli);
    struct edc_task_group *g = edc_scheduler_create_group(sched, l);
    struct edc_task *m1 = edc_scheduler_create_task(sched, "member 1", edc_channel_cli);
    struct edc_task *m2 = edc_scheduler_create_task(sched, "member 2", edc_channel_cli);
    edc_task_group_add_task(g, m1);
    edc_task_group_add_task(g, m2);

    edc_scheduler_send_signal_to_group(sched, g->group_id, TASK_SIGTERM);
    CHECK(l->state == TASK_CANCELLED, "leader cancelled");
    CHECK(m1->state == TASK_CANCELLED, "member 1 cancelled");
    CHECK(m2->state == TASK_CANCELLED, "member 2 cancelled");

    TEST("send_signal_to_group nonexistent");
    edc_scheduler_send_signal_to_group(sched, "nonexistent-gid", TASK_SIGTERM);
    CHECK(1, "signal to nonexistent group survived");

    edc_scheduler_free(sched);
    if (sess->messages) cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);
}

static void test_scheduler_signal_all(void)
{
    TEST("send_signal_to_all cancels everything");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(sess, cfg);

    struct edc_task *l1 = edc_scheduler_create_task(sched, "G1L", edc_channel_cli);
    struct edc_task_group *g1 = edc_scheduler_create_group(sched, l1);
    edc_task_group_add_task(g1, edc_scheduler_create_task(sched, "G1M", edc_channel_cli));

    struct edc_task *l2 = edc_scheduler_create_task(sched, "G2L", edc_channel_cli);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, l2);

    CHECK(g1->member_count == 2, "group 1 has 2 members");
    CHECK(g2->member_count == 1, "group 2 has 1 member");

    edc_scheduler_send_signal_to_all(sched, TASK_SIGTERM);
    CHECK(l1->state == TASK_CANCELLED, "G1 leader cancelled");
    CHECK(l2->state == TASK_CANCELLED, "G2 leader cancelled");

    edc_scheduler_free(sched);
    if (sess->messages) cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);
}

/* ── 6. 输入路由: 普通输入 attach 到 WAITING_INPUT 的 leader ── */

static int g_cmd_calls = 0;
static int g_llm_calls = 0;
static struct edc_task *g_llm_task = NULL;

static bool mock_handle_command(struct edc_scheduler *sched, struct edc_actor *actor,
                                struct edc_channel_message *message)
{
    (void)sched;
    (void)actor;
    (void)message;
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

/* 直接驱动 scheduler actor handler 投递一条 CHANNEL_RECV 消息 (模拟 TUI 输入)。 */
static void drive_input(struct edc_scheduler *sched, const char *content, long req_id)
{
    edc_actor_t *sched_actor = edc_scheduler_get_actor(sched);
    struct edc_channel_message *cm =
        edc_channel_message_new(edc_channel_cli, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, NULL, (void *)(intptr_t)req_id, cm,
                                             (edc_actor_data_free_fn)edc_channel_message_free);
    sched_actor->handler(sched_actor, msg); /* dispatches + frees msg */
}

static void test_input_routes_to_waiting_leader(void)
{
    TEST("plain input attaches to WAITING_INPUT leader (fork)");
    struct edc_config *cfg = make_config();
    struct edc_session *sess = make_session(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(sess, cfg);

    /* fork 语义: leader 在自己的(前台) group 中处于 WAITING_INPUT */
    struct edc_task *leader = edc_scheduler_create_task(sched, "fork leader", edc_channel_cli);
    struct edc_task_group *g = edc_scheduler_create_group(sched, leader);
    edc_scheduler_set_foreground(sched, g);
    edc_session_set_active_task(sess, leader);
    CHECK(leader->state == TASK_WAITING_INPUT, "leader starts WAITING_INPUT");

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "hello", 1);

    CHECK(g_llm_calls == 1, "send_llm called once (attached, not spawned)");
    CHECK(g_llm_task == leader, "input attached to leader task");
    CHECK(g_cmd_calls == 0, "shell not invoked (no new task)");
    CHECK(leader->state == TASK_LLM_PROCESSING, "leader advanced to LLM_PROCESSING");
    CHECK(leader->active_turn != NULL, "leader has active turn");

    edc_scheduler_free(sched);
    if (sess->messages)
        cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);

    TEST("plain input spawns via shell when no WAITING_INPUT task");
    cfg = make_config();
    sess = make_session(cfg);
    sched = edc_scheduler_new(sess, cfg);

    /* 已完成(非等待)的前台 task → 不再 attach, 走 shell 新建 task */
    struct edc_task *done = edc_scheduler_create_task(sched, "done", edc_channel_cli);
    struct edc_task_group *g2 = edc_scheduler_create_group(sched, done);
    edc_scheduler_set_foreground(sched, g2);
    edc_session_set_active_task(sess, done);
    edc_task_set_state(done, TASK_LLM_PROCESSING);
    edc_task_set_state(done, TASK_COMPLETED);

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "hello", 2);

    CHECK(g_cmd_calls == 1, "shell invoked when no waiting task");
    CHECK(g_llm_calls == 0, "send_llm NOT called (shell handles spawn)");

    edc_scheduler_free(sched);
    if (sess->messages)
        cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);

    TEST("slash command bypasses attach and reaches shell");
    cfg = make_config();
    sess = make_session(cfg);
    sched = edc_scheduler_new(sess, cfg);

    struct edc_task *leader2 = edc_scheduler_create_task(sched, "waiting", edc_channel_cli);
    struct edc_task_group *g3 = edc_scheduler_create_group(sched, leader2);
    edc_scheduler_set_foreground(sched, g3);
    edc_session_set_active_task(sess, leader2); /* WAITING_INPUT */

    g_cmd_calls = 0;
    g_llm_calls = 0;
    g_llm_task = NULL;
    sched->handle_command = mock_handle_command;
    sched->send_llm = mock_send_llm;

    drive_input(sched, "/task list", 3);

    CHECK(g_cmd_calls == 1, "slash command reached shell");
    CHECK(g_llm_calls == 0, "slash command did NOT attach to waiting leader");

    edc_scheduler_free(sched);
    if (sess->messages)
        cJSON_Delete(sess->messages);
    free(sess);
    free_config(cfg);
}

/* ── main ──────────────────────────────────────────────────── */

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
    test_input_routes_to_waiting_leader();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
