#include "edc_state_serializer.h"
#include "edc_session.h"
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

#define TEST_WS "/tmp/test_edc_phase3_workspace"

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(TEST_WS);
    return cfg;
}

static void clean_workspace(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_WS);
    system(cmd);
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
            if (strcmp(t->task_id, task_id) == 0) return t;
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
    struct edc_session *session = edc_session_new(cfg, "sess-rng", "Range");

    struct edc_task *task = edc_task_new("task-1", "sess-rng", "task-1", "t", edc_channel_adapter);
    task->is_group_leader = 1;
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* Fresh session (无 task) 计数为 0 */
    CHECK(edc_session_get_message_count(session) == 0, "empty session count is 0");

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-1");
    turn_add_user(turn, "user msg 1");
    turn_add_assistant(turn, "assistant msg 1");
    CHECK(edc_queue_length(&turn->messages) == 2, "turn has 2 messages");

    /* commit 到 task: 消息仍留在 turn (P4: 不再 move 到 session) */
    edc_task_add_turn(task, turn);

    CHECK(edc_queue_length(&turn->messages) == 2, "turn retains messages after commit");

    /* 计数由 turn 派生 */
    CHECK(edc_session_get_message_count(session) == 2, "session count derived from turn messages");

    cJSON *m0 = (cJSON *)turn->messages.data[turn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "role")), "user", "msg0 role user");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "user msg 1", "msg0 content");
    cJSON *m1 = (cJSON *)turn->messages.data[turn->messages.head + 1];
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
    struct edc_session *session = edc_session_new(cfg, "sess-cpt2", "Compact2");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *task = edc_task_new("task-1", "sess-cpt2", "task-1", "single", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-1");
    turn_add_user(turn, "flush query");
    turn_add_assistant(turn, "flush answer");
    edc_task_add_turn(task, turn);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds");

    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *turns_j = cJSON_GetObjectItem(t0, "turns");
    cJSON *turn_j = cJSON_GetArrayItem(turns_j, 0);

    CHECK(cJSON_GetObjectItem(turn_j, "messages") != NULL, "turn embeds messages");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(turn_j, "messages")) == 2, "embedded 2 messages");
    CHECK(cJSON_GetObjectItem(turn_j, "message_start") == NULL, "no message_start");
    CHECK(cJSON_GetObjectItem(turn_j, "message_count") == NULL, "no message_count");

    struct edc_session *session2 = edc_session_new(cfg, "sess-cpt2", "Compact2");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);

    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt = find_task(session2, "task-1");
    CHECK(rt != NULL, "task restored");
    CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored");

    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(edc_queue_length(&rturn->messages) == 2, "2 messages restored in turn");

    cJSON *rm0 = (cJSON *)rturn->messages.data[rturn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm0, "content")),
                "flush query", "msg0 content correct");

    cJSON_Delete(json);
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
    struct edc_session *session = edc_session_new(cfg, "sess-mt", "MultiTurn");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *task = edc_task_new("task-mt", "sess-mt", "task-mt", "multi turn", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);

    struct edc_turn *t0 = edc_turn_new();
    t0->task_id = strdup("task-mt");
    turn_add_user(t0, "turn0-user");
    turn_add_assistant(t0, "turn0-asst1");
    turn_add_assistant(t0, "turn0-asst2");
    edc_task_add_turn(task, t0);

    struct edc_turn *t1 = edc_turn_new();
    t1->task_id = strdup("task-mt");
    turn_add_user(t1, "turn1-user");
    turn_add_assistant(t1, "turn1-asst");
    edc_task_add_turn(task, t1);

    struct edc_turn *t2 = edc_turn_new();
    t2->task_id = strdup("task-mt");
    turn_add_user(t2, "turn2-user");
    turn_add_assistant(t2, "turn2-asst");
    edc_task_add_turn(task, t2);

    CHECK(edc_queue_length(&t0->messages) == 3, "turn0 has 3 msgs");
    CHECK(edc_queue_length(&t1->messages) == 2, "turn1 has 2 msgs");
    CHECK(edc_queue_length(&t2->messages) == 2, "turn2 has 2 msgs");

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    CHECK(edc_session_get_message_count(session) == 7, "session count = 7 (sum of turns)");

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds");

    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *t0j = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *turns_j = cJSON_GetObjectItem(t0j, "turns");
    CHECK(cJSON_GetArraySize(turns_j) == 3, "3 turns in JSON");

    for (int i = 0; i < 3; i++) {
        cJSON *tj = cJSON_GetArrayItem(turns_j, i);
        CHECK(cJSON_GetObjectItem(tj, "messages") != NULL, "turn embeds messages");
        CHECK(cJSON_GetObjectItem(tj, "message_start") == NULL, "no message_start");
        CHECK(cJSON_GetObjectItem(tj, "message_count") == NULL, "no message_count");
    }

    /* 内容直接从 turn 队列读 */
    cJSON *m = (cJSON *)t0->messages.data[t0->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), "turn0-user", "turn0 msg0");
    m = (cJSON *)t1->messages.data[t1->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), "turn1-user", "turn1 msg0");
    m = (cJSON *)t2->messages.data[t2->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m, "content")), "turn2-user", "turn2 msg0");

    cJSON_Delete(json);
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
    struct edc_session *session = edc_session_new(cfg, "sess-etf", "EmptyFlush");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *task = edc_task_new("task-etf", "sess-etf", "task-etf", "e", edc_channel_adapter);
    task->is_group_leader = 1;
    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-etf");
    edc_task_add_turn(task, turn);

    CHECK(edc_queue_length(&turn->messages) == 0, "empty turn has 0 messages");
    CHECK(edc_session_get_message_count(session) == 0, "session count 0");

    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds");
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *turns_j = cJSON_GetObjectItem(t0, "turns");
    cJSON *turn_j = cJSON_GetArrayItem(turns_j, 0);
    CHECK(cJSON_GetObjectItem(turn_j, "messages") == NULL, "empty turn has no messages array");
    cJSON_Delete(json);

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

    struct edc_session *s1 = edc_session_new(cfg, "sess-full", "Full");
    struct edc_scheduler *sc1 = edc_scheduler_new(s1, cfg);

    struct edc_task *task = edc_task_new("task-ft", "sess-full", "task-ft", "full test", edc_channel_adapter);
    task->is_group_leader = 1;
    edc_task_mark_ready(task);
    edc_task_await_exec(task);
    edc_task_mark_completed(task);
    task->output = sdsnew("final output");

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-ft");
    turn_add_user(turn, "hello");
    turn_add_assistant(turn, "world");
    edc_task_add_turn(task, turn);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(s1, group);
    s1->active_task = task;
    edc_scheduler_set_foreground(sc1, group);

    CHECK(edc_session_save_state(s1, sc1), "save_state succeeds");

    sds state_path = edc_path_join(TEST_WS, "sess-full", "session.json", NULL);
    CHECK(edc_file_exists(state_path), "session.json exists");
    sdsfree(state_path);

    edc_session_free(s1);
    edc_scheduler_free(sc1);

    struct edc_session *s2 = edc_session_new(cfg, "sess-full", "Full");
    struct edc_scheduler *sc2 = edc_scheduler_new(s2, cfg);

    CHECK(edc_session_load_state(s2, sc2), "load_state succeeds");

    struct edc_task *rt = find_task(s2, "task-ft");
    CHECK(rt != NULL, "task restored");
    CHECK(edc_task_is_completed(rt), "state COMPLETED");
    CHECK_STREQ(rt->output, "final output", "output restored");

    CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored");
    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(edc_queue_length(&rturn->messages) == 2, "2 messages restored in turn");

    cJSON *rm0 = (cJSON *)rturn->messages.data[rturn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm0, "content")), "hello", "msg0: hello");
    cJSON *rm1 = (cJSON *)rturn->messages.data[rturn->messages.head + 1];
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
