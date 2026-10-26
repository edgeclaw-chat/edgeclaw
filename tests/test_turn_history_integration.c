#include "edc_state_store.h"
#include "test_ws.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "edc_scheduler.h"
#include "edc_channel_actor.h"
#include "edc_config.h"
#include "edc_compat.h"
#include "edc_file_utils.h"
#include "utlist_fix.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
/* ── 单测的 session 仓库 ─────────────────────────────────────────
 * scheduler 现在**同包直调 store** (P3 的 `resolve_session` 端口已退休),
 * 所以测试给它的不是"解析函数"而是**仓库**: 一个只装着
 * 这些 session 的内存 manager (普通结构体, 没有构造函数 —— 与 poke 别的结构体同法)。
 * 标签为空 = 用活跃的那个 (active_index 指向第一个), 与真 store 的策略一致。
 * `ctx` 仍留给测试自己用 (它不再是读口的入参)。 */
static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* 测试进程内不释放: 与测试同生共死 */
}
/* Stub */
volatile sig_atomic_t _run_loop = 1;

/* ══════════════════════════════════════════════════════════════
 *  Test framework
 *  ══════════════════════════════════════════════════════════════ */

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while(0)
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        log_error("    FAIL: %s", msg); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while(0)
#define CHECK_STREQ(a, b, msg) CHECK(strcmp((a)?(a):"", (b)?(b):"") == 0, msg)

#define TEST_WS edc_test_workspace()

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(TEST_WS);
    return cfg;
}

static void clean_workspace(void)
{
    /* Wipe the rows, not the file: the store is open on this workspace. */
    edc_test_store_reset();
}

static void turn_add_user(struct edc_turn *turn, const char *content)
{
    edc_turn_add_user_message(turn, content);
}

static void turn_add_assistant(struct edc_turn *turn, const char *content)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "content", content);
    edc_turn_add_assistant_message(turn, msg);
}

static struct edc_task *find_task(struct edc_session *session, const char *task_id)
{
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g) {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next) {
            if (t->task_id == edc_test_id(task_id)) return t;
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: turn 运行时持有消息 (不再搬进 session->messages)
 *  ══════════════════════════════════════════════════════════════ */

static void test_turn_owns_messages(void)
{
    TEST("turn retains its messages (ownership stays on turn)");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-rng"), "Range");

    struct edc_task *task = edc_task_new(edc_test_id("task-1"), edc_test_session_id("sess-rng"), edc_test_id("task-1"), "t", edc_channel_adapter);
    task->is_group_leader = 1;
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* Fresh session (无 task) 计数为 0 */
    CHECK(edc_session_get_message_count(session) == 0, "empty session count is 0");

    struct edc_turn *turn = edc_turn_new();
    turn_add_user(turn, "user msg 1");
    turn_add_assistant(turn, "assistant msg 1");
    CHECK(turn->msg_count == 2, "turn has 2 messages");

    /* commit 到 task: 消息仍留在 turn (P4: 不再 move 到 session) */
    edc_task_add_turn(task, turn);

    CHECK(turn->msg_count == 2, "turn retains messages after commit");

    /* 计数由 turn 派生 */
    CHECK(edc_session_get_message_count(session) == 2, "session count derived from turn messages");

    cJSON *m0 = edc_turn_msg_at(turn, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "role")), "user", "msg0 role user");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "user msg 1", "msg0 content");
    cJSON *m1 = edc_turn_msg_at(turn, 1);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m1, "role")), "assistant", "msg1 role assistant");

    edc_session_free(session);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: 序列化一律内联, 反序列化恢复到 turn
 *  ══════════════════════════════════════════════════════════════ */

static void test_single_mode_roundtrip(void)
{
    TEST("single mode: turn embeds messages; roundtrip restores them");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-cpt2"), "Compact2");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new(edc_test_id("task-1"), edc_test_session_id("sess-cpt2"), edc_test_id("task-1"), "single", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);

    struct edc_turn *turn = edc_turn_new();
    turn_add_user(turn, "flush query");
    turn_add_assistant(turn, "flush answer");
    edc_task_add_turn(task, turn);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* A turn's messages are ROWS in their own table; the turn carries only the shape
     * (msg_count). Round-tripping through the store is what proves that now -- there is no
     * document to inspect, which is the point. */
    CHECK(edc_state_store_flush(session, sched), "flush succeeds");

    struct edc_session *session2 = edc_session_new(cfg, edc_test_session_id("sess-cpt2"), "Compact2");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, session2);

    bool ok = edc_state_store_load(session2, sched2);
    CHECK(ok, "load succeeds");

    struct edc_task *rt = find_task(session2, "task-1");
    CHECK(rt != NULL, "task restored");
    CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored");

    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(rturn->msg_count == 2, "the turn kept its msg_count");

    cJSON *rm0 = edc_turn_msg_at(rturn, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm0, "content")),
                "flush query", "msg0 content correct");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: 多 turn, 每个 turn 独立持有消息
 *  ══════════════════════════════════════════════════════════════ */

static void test_multi_turns(void)
{
    TEST("multi-turn: each turn owns its messages");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-mt"), "MultiTurn");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new(edc_test_id("task-mt"), edc_test_session_id("sess-mt"), edc_test_id("task-mt"), "multi turn", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);

    struct edc_turn *t0 = edc_turn_new();
    turn_add_user(t0, "turn0-user");
    turn_add_assistant(t0, "turn0-asst1");
    turn_add_assistant(t0, "turn0-asst2");
    edc_task_add_turn(task, t0);

    struct edc_turn *t1 = edc_turn_new();
    turn_add_user(t1, "turn1-user");
    turn_add_assistant(t1, "turn1-asst");
    edc_task_add_turn(task, t1);

    struct edc_turn *t2 = edc_turn_new();
    turn_add_user(t2, "turn2-user");
    turn_add_assistant(t2, "turn2-asst");
    edc_task_add_turn(task, t2);

    CHECK(t0->msg_count == 3, "turn0 has 3 msgs");
    CHECK(t1->msg_count == 2, "turn1 has 2 msgs");
    CHECK(t2->msg_count == 2, "turn2 has 2 msgs");

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    CHECK(edc_session_get_message_count(session) == 7, "session count = 7 (sum of turns)");

    CHECK(edc_state_store_flush(session, sched), "flush");

    /* The turns are rows. What used to be read out of a document is now read back from the
     * store, which is the only shape that exists. */
    {
        struct edc_session *rs = edc_session_new(cfg, edc_test_session_id("sess-mt"), "MultiTurn");
        struct edc_scheduler *rsched = edc_scheduler_new(cfg);
        install_resolver(rsched, rs);
        CHECK(edc_state_store_load(rs, rsched), "load");
        struct edc_task *rtask = find_task(rs, "task-mt");
        CHECK(rtask != NULL, "the task came back");
        CHECK(rtask && edc_queue_length(&rtask->turns) == 3, "3 turns came back");
        edc_session_free(rs);
        edc_scheduler_free(rsched);
    }

    /* 内容直接从 turn 队列读 */
    cJSON *m = edc_turn_msg_at(t0, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), "turn0-user", "turn0 msg0");
    m = edc_turn_msg_at(t1, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), "turn1-user", "turn1 msg0");
    m = edc_turn_msg_at(t2, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), "turn2-user", "turn2 msg0");

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: 空 turn
 *  ══════════════════════════════════════════════════════════════ */

static void test_empty_turn(void)
{
    TEST("empty turn: no messages, count 0");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-etf"), "EmptyFlush");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);

    struct edc_task *task = edc_task_new(edc_test_id("task-etf"), edc_test_session_id("sess-etf"), edc_test_id("task-etf"), "e", edc_channel_adapter);
    task->is_group_leader = 1;
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    struct edc_turn *turn = edc_turn_new();
    edc_task_add_turn(task, turn);

    CHECK(turn->msg_count == 0, "empty turn has 0 messages");
    CHECK(edc_session_get_message_count(session) == 0, "session count 0");

    CHECK(edc_state_store_flush(session, sched), "flush");
    /* An empty turn keeps msg_count 0 through the store -- there is nothing to embed. */
    {
        struct edc_session *rs = edc_session_new(cfg, edc_test_session_id("sess-etf"), "EmptyFlush");
        struct edc_scheduler *rsched = edc_scheduler_new(cfg);
        install_resolver(rsched, rs);
        CHECK(edc_state_store_load(rs, rsched), "load");
        struct edc_task *rtask = find_task(rs, "task-etf");
        CHECK(rtask != NULL, "the task came back");
        if (rtask && edc_queue_length(&rtask->turns) == 1)
        {
            struct edc_turn *rt = (struct edc_turn *)rtask->turns.data[rtask->turns.head];
            CHECK(rt->msg_count == 0, "the empty turn still has 0 messages");
        }
        else
            CHECK(false, "one turn came back");
        edc_session_free(rs);
        edc_scheduler_free(rsched);
    }

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 5: save_state → load_state (session.json 自包含)
 *  ══════════════════════════════════════════════════════════════ */

static void test_state_file_roundtrip(void)
{
    TEST("save_state -> load_state: turns self-contained (no history.jsonl)");
    clean_workspace();

    struct edc_config *cfg = make_config();

    struct edc_session *s1 = edc_session_new(cfg, edc_test_session_id("sess-full"), "Full");
    struct edc_scheduler *sc1 = edc_scheduler_new(cfg);
    install_resolver(sc1, s1);

    struct edc_task *task = edc_task_new(edc_test_id("task-ft"), edc_test_session_id("sess-full"), edc_test_id("task-ft"), "full test", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);
    task->output = sdsnew("final output");

    struct edc_turn *turn = edc_turn_new();
    turn_add_user(turn, "hello");
    turn_add_assistant(turn, "world");
    edc_task_add_turn(task, turn);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(s1, group);
    s1->active_task = task;
    edc_scheduler_set_foreground(s1, group);

    CHECK(edc_session_save_state(s1, sc1), "save_state succeeds");

    sds state_path = edc_path_join(TEST_WS, "edgeclaw.db", NULL);
    CHECK(edc_file_exists(state_path), "the workspace database exists");
    sdsfree(state_path);

    edc_session_free(s1);
    edc_scheduler_free(sc1);

    struct edc_session *s2 = edc_session_new(cfg, edc_test_session_id("sess-full"), "Full");
    struct edc_scheduler *sc2 = edc_scheduler_new(cfg);
    install_resolver(sc2, s2);

    CHECK(edc_session_load_state(s2, sc2), "load_state succeeds");

    struct edc_task *rt = find_task(s2, "task-ft");
    CHECK(rt != NULL, "task restored");
    CHECK(edc_task_is_completed(rt), "state COMPLETED");
    CHECK_STREQ(rt->output, "final output", "output restored");

    CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored");
    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(rturn->msg_count == 2, "2 messages restored in turn");

    cJSON *rm0 = edc_turn_msg_at(rturn, 0);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm0, "content")), "hello", "msg0: hello");
    cJSON *rm1 = edc_turn_msg_at(rturn, 1);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm1, "content")), "world", "msg1: world");

    CHECK(s2->active_task == rt, "active_task restored");

    edc_session_free(s2);
    edc_scheduler_free(sc2);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  main
 *  ══════════════════════════════════════════════════════════════ */

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    log_info("=== Turn ownership tests (P4) ===");
    log_info("");

    test_turn_owns_messages();
    test_single_mode_roundtrip();
    test_multi_turns();
    test_empty_turn();
    test_state_file_roundtrip();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
