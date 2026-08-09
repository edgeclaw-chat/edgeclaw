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

static int count_tasks(struct edc_session *session)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g) {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next) { n++; }
    }
    return n;
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
 *  Test 1: edc_session_add_turn records correct range
 *  ══════════════════════════════════════════════════════════════ */

static void test_add_turn_records_range(void)
{
    TEST("edc_session_add_turn records message range on turn");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-rng", "Range");

    /* Initial message count (system prompt) */
    size_t initial = edc_session_get_message_count(session);
    CHECK(initial >= 1, "session has at least system prompt");

    /* Create a turn with messages */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-1");
    turn_add_user(turn, "user msg 1");
    turn_add_assistant(turn, "assistant msg 1");
    size_t msg_count = edc_queue_length(&turn->messages);
    CHECK(msg_count == 2, "turn has 2 messages before flush");

    /* range not valid yet */
    CHECK(!turn->range_valid, "range_valid is false before flush");

    /* Flush — this moves messages to session->messages */
    edc_session_add_turn(session, turn);

    /* After flush, turn queue should be empty */
    CHECK(edc_queue_length(&turn->messages) == 0, "turn queue empty after flush");

    /* Range should be recorded */
    CHECK(turn->range_valid, "range_valid is true after flush");
    CHECK(turn->msg_range_start == initial, "msg_range_start == initial message count");
    CHECK(turn->msg_range_count == 2, "msg_range_count == 2");

    /* Session should have 2 more messages */
    CHECK(edc_session_get_message_count(session) == initial + 2,
          "session message count increased by 2");

    /* Verify the messages at the recorded range */
    cJSON *m0 = cJSON_GetArrayItem(session->messages, (int)turn->msg_range_start);
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "role")), "user",
                "msg at range_start is user");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")), "user msg 1",
                "msg content correct");

    cJSON *m1 = cJSON_GetArrayItem(session->messages, (int)(turn->msg_range_start + 1));
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(m1, "role")), "assistant",
                "msg at range_start+1 is assistant");

    edc_turn_free(turn);
    edc_session_free(session);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: Flushed turn uses compact range in serializer
 *  ══════════════════════════════════════════════════════════════ */

static void test_flushed_turn_compact_serialize(void)
{
    TEST("flushed turn serializes via stored range (compact path)");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-cpt2", "Compact2");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* Create task */
    struct edc_task *task = edc_task_new("task-1", "sess-cpt2", "task-1",
                                          "compact via flush", edc_channel_cli_new());
    task->is_group_leader = 1;
    edc_task_set_state(task, TASK_WAITING_INPUT);
    edc_task_set_state(task, TASK_LLM_PROCESSING);
    edc_task_set_state(task, TASK_COMPLETED);

    /* Create turn, add messages, flush to session */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-1");
    turn_add_user(turn, "flush query");
    turn_add_assistant(turn, "flush answer");
    edc_session_add_turn(session, turn);   /* flush — moves to session->messages */
    edc_task_add_turn(task, turn);          /* commit to task's turns queue */

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* Serialize */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds");

    /* Verify compact mode was used (via stored range, not embedded messages) */
    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *t0 = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *turns_j = cJSON_GetObjectItem(t0, "turns");
    cJSON *turn_j = cJSON_GetArrayItem(turns_j, 0);

    /* Should have message_start/count (compact), NOT embedded messages */
    cJSON *embedded  = cJSON_GetObjectItem(turn_j, "messages");
    cJSON *msg_start = cJSON_GetObjectItem(turn_j, "message_start");
    cJSON *msg_count = cJSON_GetObjectItem(turn_j, "message_count");

    CHECK(embedded == NULL, "no embedded messages (compact path via stored range)");
    CHECK(msg_start != NULL, "has message_start");
    CHECK(msg_count != NULL, "has message_count");
    CHECK((int)cJSON_GetNumberValue(msg_count) == 2, "message_count = 2");

    /* message_start should be >= 1 (after system prompt) */
    int start = (int)cJSON_GetNumberValue(msg_start);
    CHECK(start >= 1, "message_start >= 1");

    /* Deserialize into fresh session — must have messages at same positions */
    struct edc_session *session2 = edc_session_new(cfg, "sess-cpt2", "Compact2");
    struct edc_scheduler *sched2 = edc_scheduler_new(session2, cfg);

    /* Replicate the same messages at the same positions */
    cJSON *copy1 = cJSON_CreateObject();
    cJSON_AddStringToObject(copy1, "role", "user");
    cJSON_AddStringToObject(copy1, "content", "flush query");
    cJSON_AddItemToArray(session2->messages, copy1);

    cJSON *copy2 = cJSON_CreateObject();
    cJSON_AddStringToObject(copy2, "role", "assistant");
    cJSON_AddStringToObject(copy2, "content", "flush answer");
    cJSON_AddItemToArray(session2->messages, copy2);

    bool ok = edc_state_deserialize(json, session2, sched2);
    CHECK(ok, "deserialize succeeds");

    struct edc_task *rt = find_task(session2, "task-1");
    CHECK(rt != NULL, "task restored");
    CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored");

    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(edc_queue_length(&rturn->messages) == 2, "2 messages from compact range");

    cJSON *rm0 = (cJSON *)rturn->messages.data[rturn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm0, "content")),
                "flush query", "msg0 content correct via compact range");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_session_free(session2);
    edc_scheduler_free(sched2);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: Multi-turn contiguous ranges
 *  ══════════════════════════════════════════════════════════════ */

static void test_multi_turn_ranges(void)
{
    TEST("multi-turn contiguous ranges");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-mt", "MultiTurn");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *task = edc_task_new("task-mt", "sess-mt", "task-mt",
                                          "multi turn", edc_channel_cli_new());
    task->is_group_leader = 1;
    edc_task_set_state(task, TASK_WAITING_INPUT);
    edc_task_set_state(task, TASK_LLM_PROCESSING);
    edc_task_set_state(task, TASK_COMPLETED);

    size_t base = edc_session_get_message_count(session);

    /* Turn 0: 3 messages */
    struct edc_turn *t0 = edc_turn_new();
    t0->task_id = strdup("task-mt");
    turn_add_user(t0, "turn0-user");
    turn_add_assistant(t0, "turn0-asst1");
    turn_add_assistant(t0, "turn0-asst2");
    edc_session_add_turn(session, t0);
    edc_task_add_turn(task, t0);

    CHECK(t0->range_valid, "turn0 range recorded");
    CHECK(t0->msg_range_start == base, "turn0 starts at base");
    CHECK(t0->msg_range_count == 3, "turn0 has 3 msgs");

    /* Turn 1: 2 messages — should start right after turn0 */
    struct edc_turn *t1 = edc_turn_new();
    t1->task_id = strdup("task-mt");
    turn_add_user(t1, "turn1-user");
    turn_add_assistant(t1, "turn1-asst");
    edc_session_add_turn(session, t1);
    edc_task_add_turn(task, t1);

    CHECK(t1->range_valid, "turn1 range recorded");
    CHECK(t1->msg_range_start == base + 3, "turn1 starts at base+3");
    CHECK(t1->msg_range_count == 2, "turn1 has 2 msgs");

    /* Turn 2: 2 messages */
    struct edc_turn *t2 = edc_turn_new();
    t2->task_id = strdup("task-mt");
    turn_add_user(t2, "turn2-user");
    turn_add_assistant(t2, "turn2-asst");
    edc_session_add_turn(session, t2);
    edc_task_add_turn(task, t2);

    CHECK(t2->range_valid, "turn2 range recorded");
    CHECK(t2->msg_range_start == base + 5, "turn2 starts at base+5");
    CHECK(t2->msg_range_count == 2, "turn2 has 2 msgs");

    /* Total messages: base + 7 */
    CHECK(edc_session_get_message_count(session) == base + 7,
          "session has base+7 messages");

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(session, group);

    /* Serialize and verify ranges in JSON */
    cJSON *json = edc_state_serialize(session, sched);
    CHECK(json != NULL, "serialize succeeds");

    cJSON *tasks_j = cJSON_GetObjectItem(json, "tasks");
    cJSON *t0j = cJSON_GetArrayItem(tasks_j, 0);
    cJSON *turns_j = cJSON_GetObjectItem(t0j, "turns");
    CHECK(cJSON_GetArraySize(turns_j) == 3, "3 turns in JSON");

    /* Verify each turn has compact format */
    for (int i = 0; i < 3; i++) {
        cJSON *tj = cJSON_GetArrayItem(turns_j, i);
        CHECK(cJSON_GetObjectItem(tj, "messages") == NULL,
              "no embedded messages in turn");
        CHECK(cJSON_GetObjectItem(tj, "message_start") != NULL,
              "has message_start");
        CHECK(cJSON_GetObjectItem(tj, "message_count") != NULL,
              "has message_count");
    }

    /* Verify the content at each range */
    cJSON *msgs = session->messages;
    /* Turn 0 messages */
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
        cJSON_GetArrayItem(msgs, (int)base), "content")), "turn0-user", "turn0 msg0 content");
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
        cJSON_GetArrayItem(msgs, (int)base + 2), "content")), "turn0-asst2", "turn0 msg2 content");
    /* Turn 1 messages */
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
        cJSON_GetArrayItem(msgs, (int)base + 3), "content")), "turn1-user", "turn1 msg0 content");
    /* Turn 2 messages */
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
        cJSON_GetArrayItem(msgs, (int)base + 5), "content")), "turn2-user", "turn2 msg0 content");

    cJSON_Delete(json);
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: Empty turn flush (0 messages) — range not recorded
 *  ══════════════════════════════════════════════════════════════ */

static void test_empty_turn_flush(void)
{
    TEST("empty turn flush: range not recorded for 0 messages");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-etf", "EmptyFlush");

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-etf");

    /* Don't add any messages — flush empty turn */
    edc_session_add_turn(session, turn);

    /* Range should NOT be recorded for empty turn */
    CHECK(!turn->range_valid, "range_valid stays false for empty turn");
    CHECK(turn->msg_range_count == 0, "msg_range_count is 0");

    edc_turn_free(turn);
    edc_session_free(session);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 5: Full roundtrip via file (save_history + save_state → load)
 *  ══════════════════════════════════════════════════════════════ */

static void test_full_file_roundtrip(void)
{
    TEST("full roundtrip: save_history + save_state → load_state");
    clean_workspace();

    struct edc_config *cfg = make_config();

    /* Build session with flushed turns */
    struct edc_session *s1 = edc_session_new(cfg, "sess-full", "Full");
    struct edc_scheduler *sc1 = edc_scheduler_new(s1, cfg);

    struct edc_task *task = edc_task_new("task-ft", "sess-full", "task-ft",
                                          "full test", edc_channel_cli_new());
    task->is_group_leader = 1;
    edc_task_set_state(task, TASK_WAITING_INPUT);
    edc_task_set_state(task, TASK_LLM_PROCESSING);
    edc_task_set_state(task, TASK_COMPLETED);
    task->output = sdsnew("final output");

    /* Turn: flush to session */
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-ft");
    turn_add_user(turn, "hello");
    turn_add_assistant(turn, "world");
    edc_session_add_turn(s1, turn);
    edc_task_add_turn(task, turn);

    struct edc_task_group *group = edc_task_group_new(task);
    edc_session_add_group(s1, group);
    s1->active_task = task;
    sc1->foreground_group = group;

    /* Save history first, then state */
    CHECK(edc_session_save_history(s1), "save_history succeeds");
    CHECK(edc_session_save_state(s1, sc1), "save_state succeeds");

    /* Verify files exist */
    sds hist_path = edc_path_join(TEST_WS, "sess-full", "history.jsonl", NULL);
    sds state_path = edc_path_join(TEST_WS, "sess-full", "session.json", NULL);
    CHECK(edc_file_exists(hist_path), "history.jsonl exists");
    CHECK(edc_file_exists(state_path), "session.json exists");

    /* Clean up first session */
    cJSON_Delete(s1->messages);
    s1->messages = NULL;
    edc_session_free(s1);
    edc_scheduler_free(sc1);

    /* Now reload from files */
    struct edc_session *s2 = edc_session_new(cfg, "sess-full", "Full");
    struct edc_scheduler *sc2 = edc_scheduler_new(s2, cfg);

    CHECK(edc_session_load_state(s2, sc2), "load_state succeeds");

    /* Verify restored task */
    struct edc_task *rt = find_task(s2, "task-ft");
    CHECK(rt != NULL, "task restored");
    CHECK(rt->state == TASK_COMPLETED, "state COMPLETED");
    CHECK_STREQ(rt->output, "final output", "output restored");

    /* Verify turn messages */
    CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored");
    struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
    CHECK(edc_queue_length(&rturn->messages) == 2, "2 messages in restored turn");

    /* Verify message content — messages come from history.jsonl via compact range */
    cJSON *rm0 = (cJSON *)rturn->messages.data[rturn->messages.head];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm0, "content")),
                "hello", "msg0: hello");
    cJSON *rm1 = (cJSON *)rturn->messages.data[rturn->messages.head + 1];
    CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm1, "content")),
                "world", "msg1: world");

    /* Verify session metadata */
    CHECK(s2->active_task == rt, "active_task restored");

    sdsfree(hist_path);
    sdsfree(state_path);
    cJSON_Delete(s2->messages);
    s2->messages = NULL;
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
    log_info("=== Phase 3: Turn-History linkage tests ===");
    log_info("");

    test_add_turn_records_range();
    test_flushed_turn_compact_serialize();
    test_multi_turn_ranges();
    test_empty_turn_flush();
    test_full_file_roundtrip();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
