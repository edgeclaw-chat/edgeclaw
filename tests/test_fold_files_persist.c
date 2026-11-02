/* The tracked-file lists across the STORE, not just inside the module.
 *
 * Why this file exists: `edc_fold_files` has its own tests (test_fold_files.c) and they were
 * green while the persistence of these lists was broken in BOTH directions --
 *
 *   - the flush built a `{"modified":[...],"read":[...]}` object and tested cJSON_IsArray on
 *     it, so it never inserted a row;
 *   - the loader built a flat array of paths and handed it to a reader that expects the
 *     object, so it could never have read one back.
 *
 * Two consistent halves silently cancelling out. Neither module was wrong; the JOIN between
 * them was, and no test crossed that join. This one does: build a group in memory, flush,
 * load into a fresh session, compare the lists.
 *
 * The lists are also order-bearing (the fold merge appends in first-seen order, and the
 * rendered block is part of the cached prompt prefix), so order is asserted, not just
 * membership. */
#include "edc_compat.h"
#include "test_ws.h"
#include "edc_config.h"
#include "edc_fold_files.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "utlist_fix.h"
#include "log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t _run_loop = 1;

static int test_passed = 0, test_failed = 0;

#define TEST(name)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-56s ", name);                                                                                \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                     \
            test_failed++;                                                                                               \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("ok\n");                                                                                              \
            test_passed++;                                                                                               \
        }                                                                                                              \
    } while (0)

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm; /* the test process does not outlive this */
}

/* ── 1. both lists survive a store round trip, in order ─────────────────── */
static void test_lists_survive_roundtrip(void)
{
    TEST("both lists survive flush -> load, in order");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsdup(edc_test_workspace());

    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("ff-p"), "FF");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);

    struct edc_task *t = edc_task_new(edc_test_id("ff-t1"), s->session_id, edc_test_id("ff-t1"), "work",
                                      edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    /* Deliberately not in alphabetical order: the block is rendered from this order and it
     * sits in the cached prompt prefix, so a reordering is a real regression. */
    g->fold_files = edc_fold_files_new();
    edc_fold_files_add_modified(g->fold_files, "/src/zeta.c");
    edc_fold_files_add_modified(g->fold_files, "/src/alpha.c");
    edc_fold_files_add_read(g->fold_files, "/docs/notes.md");
    edc_fold_files_add_read(g->fold_files, "/README.md");
    edc_fold_files_add_read(g->fold_files, "/src/zeta.c"); /* also modified: see below */
    /* A path that was read AND modified belongs in `modified` only -- that rule lives in
     * edc_fold_files_merge, which the fold landing calls. Run it so the fixture has the
     * canonical shape rather than a hand-made one. */
    edc_fold_files_merge(g->fold_files, NULL);

    const size_t want_mod = edc_fold_files_modified_count(g->fold_files);
    const size_t want_read = edc_fold_files_read_count(g->fold_files);
    CHECK(want_mod == 2, "fixture has 2 modified entries");
    CHECK(want_read == 2, "fixture has 2 read entries (the also-modified one was dropped)");

    CHECK(edc_state_store_flush(s, sched), "flush");

    /* A fresh session object: that is what a restart does. */
    struct edc_session *s2 = edc_session_new(cfg, edc_test_session_id("ff-p"), "FF");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_store_load(s2, sched2), "load");

    struct edc_task_group *g2 = NULL;
    struct edc_task_group *it;
    DL_FOREACH(s2->groups, it)
    if (it->group_id == g->group_id)
        g2 = it;
    CHECK(g2 != NULL, "the group came back");

    if (g2)
    {
        CHECK(g2->fold_files != NULL, "the tracked-file lists came back (they used to be lost)");
        CHECK(edc_fold_files_modified_count(g2->fold_files) == want_mod, "same number of modified entries");
        CHECK(edc_fold_files_read_count(g2->fold_files) == want_read, "same number of read entries");
        CHECK(edc_fold_files_modified_at(g2->fold_files, 0) &&
                  strcmp(edc_fold_files_modified_at(g2->fold_files, 0), "/src/zeta.c") == 0,
              "modified order kept: zeta first");
        CHECK(edc_fold_files_modified_at(g2->fold_files, 1) &&
                  strcmp(edc_fold_files_modified_at(g2->fold_files, 1), "/src/alpha.c") == 0,
              "modified order kept: alpha second");
        CHECK(edc_fold_files_read_at(g2->fold_files, 0) &&
                  strcmp(edc_fold_files_read_at(g2->fold_files, 0), "/docs/notes.md") == 0,
              "read order kept: notes first");
        CHECK(edc_fold_files_read_at(g2->fold_files, 1) &&
                  strcmp(edc_fold_files_read_at(g2->fold_files, 1), "/README.md") == 0,
              "read order kept: README second");
    }

    edc_session_free(s2);
    edc_scheduler_free(sched2);
    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ── 2. a group with no tracked files stays empty ───────────────────────── */
static void test_empty_lists_stay_empty(void)
{
    TEST("a group with no tracked files loads with no lists");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsdup(edc_test_workspace());

    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("ff-e"), "FFE");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);

    struct edc_task *t = edc_task_new(edc_test_id("ff-t2"), s->session_id, edc_test_id("ff-t2"), "w",
                                      edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);
    /* no fold_files at all */

    CHECK(edc_state_store_flush(s, sched), "flush");

    struct edc_session *s2 = edc_session_new(cfg, edc_test_session_id("ff-e"), "FFE");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_store_load(s2, sched2), "load");

    struct edc_task_group *it;
    DL_FOREACH(s2->groups, it)
    if (it->group_id == g->group_id)
        CHECK(it->fold_files == NULL || edc_fold_files_count(it->fold_files) == 0,
              "nothing invented for a group that tracked nothing");

    edc_session_free(s2);
    edc_scheduler_free(sched2);
    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ── 3. rewriting replaces, it does not append ──────────────────────────── */
static void test_rewrite_replaces(void)
{
    TEST("a second flush replaces the rows instead of appending");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsdup(edc_test_workspace());

    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("ff-r"), "FFR");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, s);

    struct edc_task *t = edc_task_new(edc_test_id("ff-t3"), s->session_id, edc_test_id("ff-t3"), "w",
                                      edc_channel_adapter);
    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);

    g->fold_files = edc_fold_files_new();
    edc_fold_files_add_read(g->fold_files, "/one.c");
    edc_fold_files_add_read(g->fold_files, "/two.c");
    CHECK(edc_state_store_flush(s, sched), "first flush");

    /* The second fold's tracked set is smaller: the dropped path must not survive as a stale
     * row, which is what the wholesale delete per group is for. */
    edc_fold_files_free(g->fold_files);
    g->fold_files = edc_fold_files_new();
    edc_fold_files_add_read(g->fold_files, "/two.c");
    CHECK(edc_state_store_flush(s, sched), "second flush");

    struct edc_session *s2 = edc_session_new(cfg, edc_test_session_id("ff-r"), "FFR");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_resolver(sched2, s2);
    CHECK(edc_state_store_load(s2, sched2), "load");

    struct edc_task_group *it;
    DL_FOREACH(s2->groups, it)
    if (it->group_id == g->group_id)
    {
        CHECK(it->fold_files && edc_fold_files_read_count(it->fold_files) == 1,
              "one entry, not two: the dropped path is gone");
        CHECK(edc_fold_files_read_at(it->fold_files, 0) &&
                  strcmp(edc_fold_files_read_at(it->fold_files, 0), "/two.c") == 0,
              "and it is the surviving one");
    }

    edc_session_free(s2);
    edc_scheduler_free(sched2);
    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

int main(void)
{
    edc_test_workspace(); /* the disposable on-disk database, before anything writes */
    log_info("=== tracked-file lists across the store ===");

    test_lists_survive_roundtrip();
    test_empty_lists_stay_empty();
    test_rewrite_replaces();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
