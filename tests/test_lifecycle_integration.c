#include "src/edc_state_serializer.h"
#include "src/edc_session.h"
#include "src/edc_session_manager.h"
#include "src/edc_task.h"
#include "src/edc_task_group.h"
#include "src/edc_turn.h"
#include "src/edc_scheduler.h"
#include "src/edc_channel_actor.h"
#include "src/edc_config.h"
#include "src/edc_compat.h"
#include "src/edc_file_utils.h"
#include "src/utlist_fix.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

volatile sig_atomic_t _run_loop = 1;

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while(0)
#define CHECK(cond, msg) do { \
    if (!(cond)) { log_error("    FAIL: %s", msg); test_failed++; } \
    else { test_passed++; } \
} while(0)
#define CHECK_STREQ(a, b, msg) CHECK(strcmp((a)?(a):"", (b)?(b):"") == 0, msg)

#define TEST_WS "/tmp/test_edc_phase4_workspace"

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

static int count_tasks(struct edc_session *s)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(s->groups, g) {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next) { n++; }
    }
    return n;
}

static struct edc_task *find_task(struct edc_session *s, const char *tid)
{
    struct edc_task_group *g;
    DL_FOREACH(s->groups, g) {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next) {
            if (strcmp(t->task_id, tid) == 0) return t;
        }
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  Test 1: Exit save — state is persisted correctly
 *  ══════════════════════════════════════════════════════════════ */

static void test_exit_save(void)
{
    TEST("exit save: state persisted to session.json");
    clean_workspace();

    struct edc_config *cfg = make_config();

    /* Simulate a session with tasks and turns */
    struct edc_session *session = edc_session_new(cfg, "sess-exit", "ExitTest");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    struct edc_task *t1 = edc_task_new("task-exit1", "sess-exit", "task-exit1",
                                        "exit task 1", edc_channel_cli_new());
    t1->is_group_leader = 1;
    edc_task_set_state(t1, TASK_WAITING_INPUT);
    edc_task_set_state(t1, TASK_LLM_PROCESSING);
    edc_task_set_state(t1, TASK_COMPLETED);
    t1->output = sdsnew("exit output");

    struct edc_turn *turn = edc_turn_new();
    turn->task_id = strdup("task-exit1");
    /* Add messages to turn */
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", "exit query");
    edc_queue_push(&turn->messages, um);
    edc_queue_push(&turn->timestamps, sdsnew("2026-01-01T00:00:00"));

    cJSON *am = cJSON_CreateObject();
    cJSON_AddStringToObject(am, "role", "assistant");
    cJSON_AddStringToObject(am, "content", "exit answer");
    edc_queue_push(&turn->messages, am);
    edc_queue_push(&turn->timestamps, sdsnew("2026-01-01T00:00:01"));

    /* Flush turn to session (simulating real flow) */
    edc_session_add_turn(session, turn);
    edc_task_add_turn(t1, turn);

    struct edc_task_group *g1 = edc_task_group_new(t1);
    edc_session_add_group(session, g1);
    session->active_task = t1;
    sched->foreground_group = g1;

    /* Simulate exit: save_history + save_state */
    CHECK(edc_session_save_history(session), "save_history succeeds on exit");
    CHECK(edc_session_save_state(session, sched), "save_state succeeds on exit");

    /* Verify files */
    sds hp = edc_path_join(TEST_WS, "sess-exit", "history.jsonl", NULL);
    sds sp = edc_path_join(TEST_WS, "sess-exit", "session.json", NULL);
    CHECK(edc_file_exists(hp), "history.jsonl exists after exit save");
    CHECK(edc_file_exists(sp), "session.json exists after exit save");
    sdsfree(hp);
    sdsfree(sp);

    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 2: Startup load — state restored from files
 *  ══════════════════════════════════════════════════════════════ */

static void test_startup_load(void)
{
    TEST("startup load: state restored from persisted files");
    clean_workspace();

    struct edc_config *cfg = make_config();

    /* Step 1: Create and save state */
    {
        struct edc_session *s1 = edc_session_new(cfg, "sess-start", "StartTest");
        struct edc_scheduler *sc1 = edc_scheduler_new(s1, cfg);

        struct edc_task *t1 = edc_task_new("task-start1", "sess-start", "task-start1",
                                            "startup task", edc_channel_cli_new());
        t1->is_group_leader = 1;
        edc_task_set_state(t1, TASK_WAITING_INPUT);
        edc_task_set_state(t1, TASK_LLM_PROCESSING);
        edc_task_set_state(t1, TASK_COMPLETED);
        t1->output = sdsnew("startup output");
        t1->completion_seq = 99;

        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup("task-start1");
        cJSON *um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        cJSON_AddStringToObject(um, "content", "startup query");
        edc_queue_push(&turn->messages, um);
        edc_queue_push(&turn->timestamps, sdsnew("2026-01-01T00:00:00"));
        edc_session_add_turn(s1, turn);
        edc_task_add_turn(t1, turn);

        struct edc_task_group *g1 = edc_task_group_new(t1);
        edc_session_add_group(s1, g1);
        s1->active_task = t1;
        s1->next_completion_seq = 100;
        sc1->foreground_group = g1;

        edc_session_save_history(s1);
        edc_session_save_state(s1, sc1);

        cJSON_Delete(s1->messages);
        s1->messages = NULL;
        edc_session_free(s1);
        edc_scheduler_free(sc1);
    }

    /* Step 2: Simulate restart — create new session and load */
    {
        struct edc_session *s2 = edc_session_new(cfg, "sess-start", "StartTest");
        struct edc_scheduler *sc2 = edc_scheduler_new(s2, cfg);

        /* Load state (simulating what edc_agent_new does) */
        bool loaded = edc_session_load_state(s2, sc2);
        CHECK(loaded, "load_state succeeds on startup");

        /* Verify restored state */
        CHECK(count_tasks(s2) == 1, "1 task restored on startup");
        struct edc_task *rt = find_task(s2, "task-start1");
        CHECK(rt != NULL, "task found after startup load");
        CHECK(rt->state == TASK_COMPLETED, "state COMPLETED");
        CHECK_STREQ(rt->output, "startup output", "output restored");
        CHECK(rt->completion_seq == 99, "completion_seq restored");
        CHECK(s2->next_completion_seq == 100, "next_completion_seq restored");
        CHECK(s2->active_task == rt, "active_task restored");
        CHECK(sc2->foreground_group != NULL, "foreground_group restored");

        /* Verify turn messages loaded from history.jsonl */
        CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored");
        struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
        CHECK(edc_queue_length(&rturn->messages) == 1, "1 message in restored turn");
        cJSON *rm0 = (cJSON *)rturn->messages.data[rturn->messages.head];
        CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(rm0, "content")),
                    "startup query", "message content correct");

        cJSON_Delete(s2->messages);
        s2->messages = NULL;
        edc_session_free(s2);
        edc_scheduler_free(sc2);
    }

    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 3: Session switch saves old state
 *  ══════════════════════════════════════════════════════════════ */

static void test_switch_save(void)
{
    TEST("session switch: old session state saved");
    clean_workspace();

    struct edc_config *cfg = make_config();

    /* Create session A with state */
    struct edc_session *sA = edc_session_new(cfg, "sess-A", "SessionA");
    struct edc_scheduler *scA = edc_scheduler_new(sA, cfg);

    struct edc_task *tA = edc_task_new("task-A", "sess-A", "task-A",
                                        "task in A", edc_channel_cli_new());
    tA->is_group_leader = 1;
    edc_task_set_state(tA, TASK_WAITING_INPUT);
    edc_task_set_state(tA, TASK_LLM_PROCESSING);
    edc_task_set_state(tA, TASK_COMPLETED);
    tA->output = sdsnew("output from A");

    struct edc_task_group *gA = edc_task_group_new(tA);
    edc_session_add_group(sA, gA);
    sA->active_task = tA;
    scA->foreground_group = gA;

    /* Save session A's state */
    edc_session_save_history(sA);
    edc_session_save_state(sA, scA);

    /* Create session B (simulating switch) */
    struct edc_session *sB = edc_session_new(cfg, "sess-B", "SessionB");
    struct edc_scheduler *scB = edc_scheduler_new(sB, cfg);

    struct edc_task *tB = edc_task_new("task-B", "sess-B", "task-B",
                                        "task in B", edc_channel_cli_new());
    tB->is_group_leader = 1;
    edc_task_set_state(tB, TASK_WAITING_INPUT);
    edc_task_set_state(tB, TASK_LLM_PROCESSING);
    edc_task_set_state(tB, TASK_COMPLETED);
    tB->output = sdsnew("output from B");

    struct edc_task_group *gB = edc_task_group_new(tB);
    edc_session_add_group(sB, gB);

    edc_session_save_history(sB);
    edc_session_save_state(sB, scB);

    /* Verify session A's file still has correct data */
    struct edc_session *sA2 = edc_session_new(cfg, "sess-A", "SessionA");
    struct edc_scheduler *scA2 = edc_scheduler_new(sA2, cfg);
    CHECK(edc_session_load_state(sA2, scA2), "session A state loads correctly");
    struct edc_task *rtA = find_task(sA2, "task-A");
    CHECK(rtA != NULL, "task-A still in session A");
    CHECK_STREQ(rtA->output, "output from A", "session A output intact");

    /* Verify session B's file has correct data */
    struct edc_session *sB2 = edc_session_new(cfg, "sess-B", "SessionB");
    struct edc_scheduler *scB2 = edc_scheduler_new(sB2, cfg);
    CHECK(edc_session_load_state(sB2, scB2), "session B state loads correctly");
    struct edc_task *rtB = find_task(sB2, "task-B");
    CHECK(rtB != NULL, "task-B in session B");
    CHECK_STREQ(rtB->output, "output from B", "session B output intact");

    /* Cleanup */
    cJSON_Delete(sA->messages); sA->messages = NULL;
    cJSON_Delete(sA2->messages); sA2->messages = NULL;
    cJSON_Delete(sB->messages); sB->messages = NULL;
    cJSON_Delete(sB2->messages); sB2->messages = NULL;
    edc_session_free(sA); edc_scheduler_free(scA);
    edc_session_free(sA2); edc_scheduler_free(scA2);
    edc_session_free(sB); edc_scheduler_free(scB);
    edc_session_free(sB2); edc_scheduler_free(scB2);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 4: Repeated exit/restart cycle
 *  ══════════════════════════════════════════════════════════════ */

static void test_repeated_cycle(void)
{
    TEST("repeated exit/restart: state remains consistent");
    clean_workspace();

    struct edc_config *cfg = make_config();

    for (int cycle = 0; cycle < 3; cycle++) {
        /* Create session, add state, save, free */
        struct edc_session *s = edc_session_new(cfg, "sess-cycle", "Cycle");
        struct edc_scheduler *sc = edc_scheduler_new(s, cfg);

        /* Load previous state if exists */
        edc_session_load_state(s, sc);

        int expected_tasks = cycle; /* each cycle adds 1 task */

        /* Add a new task */
        char tid[32], tdesc[64];
        snprintf(tid, sizeof(tid), "task-cycle%d", cycle);
        snprintf(tdesc, sizeof(tdesc), "cycle %d task", cycle);

        struct edc_task *t = edc_task_new(tid, "sess-cycle",
                                           cycle == 0 ? tid : "task-cycle0",
                                           tdesc, cycle == 0 ? edc_channel_cli_new() : NULL);
        if (cycle == 0) t->is_group_leader = 1;
        edc_task_set_state(t, TASK_WAITING_INPUT);
        edc_task_set_state(t, TASK_LLM_PROCESSING);
        edc_task_set_state(t, TASK_COMPLETED);

        /* On first cycle, create group */
        if (cycle == 0) {
            struct edc_task_group *g = edc_task_group_new(t);
            edc_session_add_group(s, g);
        } else {
            /* Add to existing group */
            struct edc_task_group *g = NULL;
            DL_FOREACH(s->groups, g) { break; }
            if (g) edc_task_group_add_task(g, t);
        }

        /* Verify task count after load+add */
        CHECK(count_tasks(s) == expected_tasks + 1,
              "correct task count after cycle");

        /* Save and free */
        edc_session_save_history(s);
        edc_session_save_state(s, sc);

        cJSON_Delete(s->messages);
        s->messages = NULL;
        edc_session_free(s);
        edc_scheduler_free(sc);
    }

    /* Final load: should have 3 tasks */
    struct edc_session *sf = edc_session_new(cfg, "sess-cycle", "Cycle");
    struct edc_scheduler *scf = edc_scheduler_new(sf, cfg);
    CHECK(edc_session_load_state(sf, scf), "final load succeeds");
    CHECK(count_tasks(sf) == 3, "3 tasks after 3 cycles");

    /* Verify all tasks exist */
    CHECK(find_task(sf, "task-cycle0") != NULL, "cycle0 task exists");
    CHECK(find_task(sf, "task-cycle1") != NULL, "cycle1 task exists");
    CHECK(find_task(sf, "task-cycle2") != NULL, "cycle2 task exists");

    cJSON_Delete(sf->messages); sf->messages = NULL;
    edc_session_free(sf);
    edc_scheduler_free(scf);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 5: Save on empty session (no state to persist)
 *  ══════════════════════════════════════════════════════════════ */

static void test_save_empty(void)
{
    TEST("save empty session: save_state succeeds gracefully");
    clean_workspace();

    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, "sess-empty2", "Empty2");
    struct edc_scheduler *sched = edc_scheduler_new(session, cfg);

    /* Save with no tasks/groups */
    CHECK(edc_session_save_state(session, sched), "save_state on empty session succeeds");

    /* Load back */
    struct edc_session *s2 = edc_session_new(cfg, "sess-empty2", "Empty2");
    struct edc_scheduler *sc2 = edc_scheduler_new(s2, cfg);
    CHECK(edc_session_load_state(s2, sc2), "load_state on empty session succeeds");
    CHECK(count_tasks(s2) == 0, "0 tasks");

    cJSON_Delete(session->messages); session->messages = NULL;
    cJSON_Delete(s2->messages); s2->messages = NULL;
    edc_session_free(session); edc_scheduler_free(sched);
    edc_session_free(s2); edc_scheduler_free(sc2);
    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 6: session.json exists but history.jsonl is missing
 *  ══════════════════════════════════════════════════════════════ */

static void test_missing_history(void)
{
    TEST("session.json with missing history.jsonl: graceful degradation");
    clean_workspace();

    struct edc_config *cfg = make_config();

    /* Step 1: create and save both state and history */
    {
        struct edc_session *s = edc_session_new(cfg, "sess-mh", "MissingHist");
        struct edc_scheduler *sc = edc_scheduler_new(s, cfg);

        struct edc_task *t = edc_task_new("t-mh", "sess-mh", "t-mh",
                                           "missing hist task", edc_channel_cli_new());
        t->is_group_leader = 1;
        edc_task_set_state(t, TASK_WAITING_INPUT);
        edc_task_set_state(t, TASK_LLM_PROCESSING);
        edc_task_set_state(t, TASK_COMPLETED);

        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup("t-mh");
        cJSON *um = cJSON_CreateObject();
        cJSON_AddStringToObject(um, "role", "user");
        cJSON_AddStringToObject(um, "content", "will be deleted");
        edc_queue_push(&turn->messages, um);
        edc_queue_push(&turn->timestamps, sdsnew("2026-01-01T00:00:00"));
        edc_session_add_turn(s, turn);
        edc_task_add_turn(t, turn);

        struct edc_task_group *g = edc_task_group_new(t);
        edc_session_add_group(s, g);

        edc_session_save_history(s);
        edc_session_save_state(s, sc);

        cJSON_Delete(s->messages);
        s->messages = NULL;
        edc_session_free(s);
        edc_scheduler_free(sc);
    }

    /* Step 2: delete history.jsonl, keep session.json */
    sds hp = edc_path_join(TEST_WS, "sess-mh", "history.jsonl", NULL);
    CHECK(edc_file_exists(hp), "history.jsonl exists before delete");
    unlink(hp);
    CHECK(!edc_file_exists(hp), "history.jsonl deleted");
    sdsfree(hp);

    sds sp = edc_path_join(TEST_WS, "sess-mh", "session.json", NULL);
    CHECK(edc_file_exists(sp), "session.json still exists");
    sdsfree(sp);

    /* Step 3: load — should not crash, should restore task structure
     * but turns will have empty messages (since history is gone) */
    {
        struct edc_session *s2 = edc_session_new(cfg, "sess-mh", "MissingHist");
        struct edc_scheduler *sc2 = edc_scheduler_new(s2, cfg);

        /* load_state should succeed (task structure is in session.json) */
        bool loaded = edc_session_load_state(s2, sc2);
        CHECK(loaded, "load_state succeeds even with missing history");

        /* Task structure restored */
        CHECK(count_tasks(s2) == 1, "1 task restored from session.json");
        struct edc_task *rt = find_task(s2, "t-mh");
        CHECK(rt != NULL, "task found despite missing history");
        CHECK(rt->state == TASK_COMPLETED, "state preserved");

        /* Turn exists but messages are empty (history.jsonl was missing) */
        CHECK(edc_queue_length(&rt->turns) == 1, "1 turn restored (structure only)");
        struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
        /* The turn's message_range points beyond the truncated messages array,
         * so the turn should have 0 messages. */
        CHECK(edc_queue_length(&rturn->messages) == 0,
              "turn has 0 messages (history missing, range out of bounds)");

        cJSON_Delete(s2->messages);
        s2->messages = NULL;
        edc_session_free(s2);
        edc_scheduler_free(sc2);
    }

    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  Test 7: session.json with turn range beyond history.jsonl
 *  ══════════════════════════════════════════════════════════════ */

static void test_truncated_history(void)
{
    TEST("session.json with turn range beyond truncated history: graceful");
    clean_workspace();

    struct edc_config *cfg = make_config();

    /* Step 1: create and save */
    {
        struct edc_session *s = edc_session_new(cfg, "sess-trunc", "Truncated");
        struct edc_scheduler *sc = edc_scheduler_new(s, cfg);

        struct edc_task *t = edc_task_new("t-trunc", "sess-trunc", "t-trunc",
                                           "trunc task", edc_channel_cli_new());
        t->is_group_leader = 1;
        edc_task_set_state(t, TASK_WAITING_INPUT);
        edc_task_set_state(t, TASK_LLM_PROCESSING);
        edc_task_set_state(t, TASK_COMPLETED);

        /* Add 3 messages to create a turn with range [1, 4) */
        struct edc_turn *turn = edc_turn_new();
        turn->task_id = strdup("t-trunc");
        for (int i = 0; i < 3; i++) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "user");
            char buf[32];
            snprintf(buf, sizeof(buf), "msg %d", i);
            cJSON_AddStringToObject(m, "content", buf);
            edc_queue_push(&turn->messages, m);
            edc_queue_push(&turn->timestamps, sdsnew("2026-01-01T00:00:00"));
        }
        edc_session_add_turn(s, turn);
        edc_task_add_turn(t, turn);

        struct edc_task_group *g = edc_task_group_new(t);
        edc_session_add_group(s, g);

        edc_session_save_history(s);
        edc_session_save_state(s, sc);

        cJSON_Delete(s->messages);
        s->messages = NULL;
        edc_session_free(s);
        edc_scheduler_free(sc);
    }

    /* Step 2: verify file sizes, then truncate history.jsonl to just system prompt */
    sds hp = edc_path_join(TEST_WS, "sess-trunc", "history.jsonl", NULL);
    CHECK(edc_file_exists(hp), "history.jsonl exists");

    /* Replace history.jsonl with just a system prompt (simulating compression) */
    FILE *f = fopen(hp, "w");
    fprintf(f, "{\"role\":\"system\",\"content\":\"truncated system prompt\"}\n");
    fclose(f);
    sdsfree(hp);

    /* Step 3: load — turns should have fewer/no messages */
    {
        struct edc_session *s2 = edc_session_new(cfg, "sess-trunc", "Truncated");
        struct edc_scheduler *sc2 = edc_scheduler_new(s2, cfg);

        bool loaded = edc_session_load_state(s2, sc2);
        CHECK(loaded, "load_state succeeds with truncated history");

        struct edc_task *rt = find_task(s2, "t-trunc");
        CHECK(rt != NULL, "task restored");
        CHECK(edc_queue_length(&rt->turns) == 1, "1 turn");

        struct edc_turn *rturn = (struct edc_turn *)rt->turns.data[rt->turns.head];
        /* The turn had message_range [1, 4), but history now has only
         * 2 messages total (system [0] + history line [1]).
         * So only 1 message (index 1) can be restored; indices 2,3 are truncated. */
        CHECK(edc_queue_length(&rturn->messages) == 1,
              "turn has 1 message (partial range within truncated history)");

        cJSON_Delete(s2->messages);
        s2->messages = NULL;
        edc_session_free(s2);
        edc_scheduler_free(sc2);
    }

    edc_config_free(cfg);
    clean_workspace();
}

/* ══════════════════════════════════════════════════════════════
 *  main
 *  ══════════════════════════════════════════════════════════════ */

int main(void)
{
    log_info("=== Phase 4: Lifecycle integration tests ===");
    log_info("");

    test_exit_save();
    test_startup_load();
    test_switch_save();
    test_repeated_cycle();
    test_save_empty();
    test_missing_history();
    test_truncated_history();

    log_info("");
    log_info("=== Results: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
