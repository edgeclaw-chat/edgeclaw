/* Cross-session task-id ownership (SQLite: one primary-key lookup).
 *
 * This file used to verify the lazy uthash index that was built by parsing each unloaded
 * session's session.json (and, before v3, a `tasks.idx` sidecar). Both are gone: the owner
 * of a task is now a single `SELECT session_id FROM tasks WHERE task_id=?`.
 *
 * Rewritten rather than deleted because the CONTRACT it guards is still real and still
 * load-bearing -- a cross-session dependency needs to find the session that owns a task
 * without loading every session first. What changed is only the mechanism, so the
 * assertions moved from "the index was filled by parsing a file" to "the lookup answers
 * correctly, and keeps answering after the persisted state changes".
 *
 * Verified here:
 *   a. a task in an UNLOADED session still resolves to its owner;
 *   b. an in-memory (loaded) session wins over the database, so a task created but not
 *      yet flushed is found;
 *   c. an unknown id resolves to NULL;
 *   d. forgetting a session makes its tasks unresolvable (the lookup reads live rows,
 *      not a cache that could go stale).
 */

#include "edc_channel_actor.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t _run_loop = 1;

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

/* A manager over the same workspace, as a restarted process would build it. */
static struct edc_session_manager *open_sm(struct edc_config *cfg)
{
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    return sm;
}

/* Give a task the input it needs to survive a flush: a task with no turn and no tool
 * ledger is dropped by the serializer's persist predicate. */
static void give_input(struct edc_task *t)
{
    struct edc_turn *turn = edc_turn_new();
    turn->task_id = t->task_id;
    edc_turn_add_user_message(turn, "input");
    t->active_turn = turn;
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    clean_workspace();

    struct edc_config *cfg = make_config();
    if (!edc_state_store_open(cfg))
    {
        log_error("cannot open the store");
        return 1;
    }

    struct edc_session_manager *sm = open_sm(cfg);
    struct edc_session *sa = edc_session_manager_get_active(sm);
    sa->state_loaded = true;
    struct edc_session *sb = edc_session_manager_create_ex(sm, "B", NULL);
    sm->active_index = edc_session_manager_index_of(sm, sa->session_id);

    edc_actor_t *channel_actor = edc_channel_actor_new();
    struct edc_scheduler *sched = edc_scheduler_new(cfg);

    struct edc_task *bt = edc_scheduler_task_new(sched, sb, "b-task", edc_channel_adapter);
    edc_scheduler_create_group(sched, sb, bt);
    give_input(bt);
    edc_task_mark_interrupted(bt);
    CHECK(edc_session_save_state(sb, sched), "B state saved");

    sds bt_id = sdsnew(edc_id_str(bt->task_id));

    /* Also give A a task, so "the active session does not own it" is a real distinction. */
    struct edc_task *at = edc_scheduler_task_new(sched, sa, "a-task", edc_channel_adapter);
    edc_scheduler_create_group(sched, sa, at);
    give_input(at);
    edc_task_mark_interrupted(at);
    CHECK(edc_session_save_state(sa, sched), "A state saved");

    TEST("a task in an unloaded session resolves to its owner");
    {
        struct edc_config *c = make_config();
        struct edc_session_manager *m = open_sm(c);
        struct edc_session *b = edc_session_manager_find(m, "B");
        CHECK(b != NULL && !b->state_loaded, "B present but unloaded");
        CHECK(edc_session_manager_session_for_task(m, (int64_t)strtoll(bt_id, NULL, 10)) == b,
              "owner resolved by a primary-key lookup (no parsing)");
        CHECK(edc_session_manager_session_for_task(m, (int64_t)strtoll(bt_id, NULL, 10)) == b, "a repeated lookup is stable");
        edc_session_manager_free(m);
        edc_config_free(c);
    }

    TEST("an in-memory session wins over the database");
    {
        /* A task that exists only in memory (nothing flushed it) must still be found. */
        struct edc_config *c = make_config();
        struct edc_session_manager *m = open_sm(c);
        struct edc_session *a = edc_session_manager_get_active(m);
        a->state_loaded = true;
        struct edc_task *fresh = edc_task_new(edc_test_id("never-flushed"), a->session_id, edc_test_id("g-x"), "fresh",
                                              edc_channel_adapter);
        fresh->is_group_leader = 1;
        edc_queue_init(&fresh->turns);
        edc_session_add_group(a, edc_task_group_new(fresh));
        CHECK(edc_session_manager_session_for_task(m, edc_test_id("never-flushed")) == a,
              "an unflushed in-memory task still resolves");
        edc_session_manager_free(m);
        edc_config_free(c);
    }

    TEST("an unknown id resolves to NULL");
    {
        struct edc_config *c = make_config();
        struct edc_session_manager *m = open_sm(c);
        CHECK(edc_session_manager_session_for_task(m, edc_test_id("no-such-task-at-all")) == NULL,
              "unknown id -> NULL");
        edc_session_manager_free(m);
        edc_config_free(c);
    }

    TEST("forgetting a session makes its tasks unresolvable (no stale cache)");
    {
        sds b_sid = sdsnew(sb->session_id);
        CHECK(edc_state_store_forget(b_sid), "forget B");

        struct edc_config *c = make_config();
        struct edc_session_manager *m = open_sm(c);
        /* The list IS the database now, so forgetting a session also removes it from the
         * list. (Before, sessions.json still carried B and only its state was gone -- a
         * shell that could not be loaded and that nothing could ever populate again.) */
        struct edc_session *b = edc_session_manager_find(m, "B");
        CHECK(b == NULL, "B is gone from the list as well");
        CHECK(edc_session_manager_session_for_task(m, (int64_t)strtoll(bt_id, NULL, 10)) == NULL,
              "B's task no longer resolves to anyone");
        edc_session_manager_free(m);
        edc_config_free(c);
        sdsfree(b_sid);
    }

    edc_scheduler_free(sched);
    edc_channel_actor_free(channel_actor);
    edc_session_manager_free(sm);
    edc_config_free(cfg);
    edc_state_store_close();
    clean_workspace();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
