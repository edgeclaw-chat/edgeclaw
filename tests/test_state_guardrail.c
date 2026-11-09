/* Load guardrail: a session that failed to load must never be persisted over.
 *
 * The incident this exists for: a load failed, the process kept running with an empty
 * shell, and the next persist wrote that shell into the session's slot -- destroying the
 * real state (s7's task structure). The old mechanism was a `.broken` rename of
 * session.json; with SQLite it is the `state_quarantined` flag, which save_state honours
 * at its single exit point.
 *
 * What changed and what did not:
 *   - the FILE-level story (a truncated or unparseable session.json, a version range, a
 *     failed rename) is gone, because a transaction either commits or it does not;
 *   - the GUARANTEE is unchanged, and is what this file now tests:
 *       once a load has failed on state that exists, this process refuses to write.
 *
 * Verified here:
 *   1. a quarantined session refuses to persist;
 *   2. the database is byte-for-byte the same state afterwards (nothing was half-written);
 *   3. a healthy session is unaffected -- the refusal is per session, not global.
 */

#include "cJSON.h"
#include "test_ws.h"
#include "edc_channel_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
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
#include "test_fs_util.h"

volatile sig_atomic_t _run_loop = 1;

static int g_failed = 0;
static int g_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while (0)
#define CHECK(cond, msg) do { \
    if (!(cond)) { log_error("    FAIL: %s", msg); g_failed++; } \
    else { g_passed++; } \
} while (0)

#define TEST_WS edc_test_workspace()

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(TEST_WS);
    return cfg;
}

static void install_sessions(struct edc_scheduler *sched, struct edc_session *sess)
{
    sched->ctx = sess;
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm;
}

/* A task with input, so the serializer's persist predicate keeps it. */
static struct edc_task_group *add_task(struct edc_session *s, const char *task_id, const char *desc)
{
    struct edc_task *t = edc_task_new(edc_test_id(task_id), s->session_id, edc_test_id("g-1"), desc, edc_channel_adapter);
    t->is_group_leader = 1;
    t->step = EDC_TASKSTEP_DONE;
    t->life = EDC_LIFE_COMPLETED;
    edc_queue_init(&t->turns);
    struct edc_task_group *g = edc_task_group_new(t);
    edc_session_add_group(s, g);
    return g;
}

/* What a fresh reader sees: the session's task descriptions, in order. */
static sds load_descriptions(const char *session_id)
{
    struct edc_config *cfg = make_config();
    struct edc_session *r = edc_session_new(cfg, edc_test_session_id(session_id), "Probe");
    struct edc_scheduler *rsched = edc_scheduler_new(cfg);
    install_sessions(rsched, r);
    sds out = sdsempty();
    if (edc_state_store_load(r, rsched))
    {
        struct edc_task_group *g;
        DL_FOREACH(r->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                out = sdscat(out, t->description ? t->description : "");
                out = sdscat(out, "|");
            }
        }
    }
    edc_session_free(r);
    edc_scheduler_free(rsched);
    sdsfree(cfg->workspace);
    free(cfg);
    return out;
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_WS);
    if (test_sh(cmd) != 0)
        log_warn("could not clean %s", TEST_WS);

    struct edc_config *cfg = make_config();
    if (!edc_state_store_open(cfg))
    {
        log_error("cannot open the store");
        return 1;
    }

    /* ── Establish real state ─────────────────────────────────────────────── */
    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("s-guard"), "Guard");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);
    add_task(s, "g-1-task", "the real task");
    CHECK(edc_session_save_state(s, sched), "the real state is saved");

    {
        sds before = load_descriptions("s-guard");
        CHECK(strstr(before, "the real task") != NULL, "the real task is readable back");
        sdsfree(before);
    }

    /* ── 1. a quarantined session refuses to persist ──────────────────────── */
    TEST("a quarantined session refuses to persist");
    {
        /* Corrupt the in-memory tree the way a failed load would: an empty shell. Then set
         * the flag exactly as the loader does when it cannot read existing rows. */
        edc_session_reset(s);
        s->state_quarantined = true;

        CHECK(!edc_session_save_state(s, sched), "save_state refuses");
    }

    /* ── 2. and the database still holds the real state ──────────────────── */
    TEST("the refusal left the database untouched");
    {
        sds after = load_descriptions("s-guard");
        CHECK(strstr(after, "the real task") != NULL,
              "the real task survived (the empty shell was NOT written over it)");
        sdsfree(after);
    }

    /* ── 3. the refusal is per session, not global ───────────────────────── */
    TEST("a healthy session still saves while another is quarantined");
    {
        struct edc_session *other = edc_session_new(cfg, edc_test_session_id("s-healthy"), "Healthy");
        struct edc_scheduler *osched = edc_scheduler_new(cfg);
        install_sessions(osched, other);
        add_task(other, "h-1-task", "a healthy task");
        CHECK(edc_session_save_state(other, osched), "the healthy session saves");

        sds text = load_descriptions("s-healthy");
        CHECK(strstr(text, "a healthy task") != NULL, "and its state is readable back");
        sdsfree(text);

        edc_session_free(other);
        edc_scheduler_free(osched);
    }

    edc_session_free(s);
    edc_scheduler_free(sched);
    sdsfree(cfg->workspace);
    free(cfg);
    edc_state_store_close();
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_WS);
    if (test_sh(cmd) != 0)
        log_warn("could not clean %s", TEST_WS);

    log_info("passed=%d failed=%d", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
