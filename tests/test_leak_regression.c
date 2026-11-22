/* Regression for the cJSON ownership fixes on 2026-11-20.
 *
 * Two leaks were found in a snapshot of the live process:
 *  1. state_store's resume-ledger load: cJSON_Parse(args) was freed only when
 *     edc_task_tool_call_add failed, but that function DUPLICATES its argument.
 *     Every restored tool call leaked one parsed cJSON tree.
 *  2. the sidebar snapshot: edc_task_user_payload already returns a private
 *     duplicate, but sched_handle_task_snapshot duplicated it again and never
 *     freed the first copy -- one tree per task per snapshot push.
 *
 * A counting cJSON allocator makes both deterministic: install hooks, do the
 * operation N times, assert the number of LIVE cJSON allocations does not grow.
 * Hooks are installed at process start, before anything allocates, so every
 * cJSON block uses one consistent allocator. Anti-vacuous guards assert the
 * operation actually allocated (otherwise "no growth" is a vacuous pass).
 *
 * NOT covered here (honest scope):
 *  - edc_https_request_new's teardown race is a forced-exit path that cannot be
 *    reproduced deterministically in a unit test.
 */

#include "test_ws.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "cJSON.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── counting cJSON allocator ──────────────────────────────────────────────── */
static long g_live = 0;
static long g_allocs = 0;
static int g_hooks_on = 0;
static void *counting_malloc(size_t sz)
{
    void *p = malloc(sz);
    if (p && g_hooks_on)
    {
        g_live++;
        g_allocs++;
    }
    return p;
}
static void counting_free(void *p)
{
    if (p && g_hooks_on)
        g_live--;
    free(p);
}

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            g_pass++;                                                                                                  \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL: %s\n", msg);                                                                        \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(edc_test_workspace());
    return cfg;
}

static void install_resolver(struct edc_scheduler *sched, struct edc_session *sess)
{
    struct edc_session_manager *sm = g_new0(struct edc_session_manager, 1);
    sm->sessions = g_new0(struct edc_session *, 1);
    sm->sessions[0] = sess;
    sm->session_count = sess ? 1 : 0;
    sm->active_index = 0;
    sched->sessions = sm;
}

static sds g_snapshot = NULL;
static int g_pushes = 0;
static void capture_snapshot(struct edc_scheduler *sched, sds json)
{
    (void)sched;
    if (g_snapshot)
        sdsfree(g_snapshot);
    g_snapshot = json;
    g_pushes++;
}
static void request_snapshot(struct edc_scheduler *sched, struct edc_session *session)
{
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, NULL, NULL,
                                             sdsnew(session ? edc_id_str(session->session_id) : ""),
                                             (edc_actor_data_free_fn)sdsfree);
    sched->self->handler(sched->self, msg);
}

/* A task whose user message carries a folded-paste payload -- the exact shape
 * edc_task_user_payload reads and the snapshot used to leak. */
static struct edc_task *make_task_with_payload(struct edc_session *session, int64_t id)
{
    struct edc_task *t = edc_task_new(id, session->session_id, id, "payload task", edc_channel_adapter);
    struct edc_turn *turn = edc_turn_new_for(session->session_id, id, id, 0);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", "typed line");
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "line-1", "folded original");
    cJSON_AddItemToObject(msg, "payload", payload);
    edc_turn_add_message(turn, msg); /* takes ownership */
    t->active_turn = turn;
    return t;
}

/* Repeated snapshots must not leak a cJSON tree per task per push. */
static void test_snapshot_no_leak(void)
{
    fprintf(stderr, "TEST: repeated snapshots do not leak a cJSON tree\n");
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-leak-snap"), "LeakSnap");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_resolver(sched, session);
    sched->snapshot_reply = capture_snapshot;
    g_pushes = 0;

    struct edc_task *t = make_task_with_payload(session, edc_test_id("1"));
    struct edc_task_group *group = edc_task_group_new(t);
    edc_session_add_group(session, group);

    g_hooks_on = 1;
    g_live = g_allocs = 0;
    request_snapshot(sched, session); /* warm-up: establish steady state */
    g_live = g_allocs = 0;            /* measure only the repeated pushes */
    for (int i = 0; i < 20; i++)
        request_snapshot(sched, session);
    long leaked = g_live;
    g_hooks_on = 0;
    CHECK(g_pushes == 21, "anti-vacuous: 21 snapshots actually produced");
    CHECK(g_allocs > 0, "anti-vacuous: snapshots allocate cJSON nodes");
    CHECK(leaked == 0, "20 snapshot pushes leave zero live cJSON nodes (no per-task payload leak)");

    if (g_snapshot)
        sdsfree(g_snapshot);
    g_snapshot = NULL;
    edc_session_free(session);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* The resume-ledger load must free the parsed args tree it hands to
 * edc_task_tool_call_add (which duplicates it). Repeated load of a session with
 * a tool call must not leak one tree per load. */
static void test_resume_ledger_no_leak(void)
{
    fprintf(stderr, "TEST: loading the resume ledger does not leak parsed args\n");
    /* Build one session with a tool call through the real store. */
    struct edc_config *cfg = make_config();
    struct edc_session *session = edc_session_new(cfg, edc_test_session_id("sess-ledger"), "Ledger");
    struct edc_task *t = edc_task_new(edc_test_id("1"), session->session_id, edc_test_id("1"), "leader",
                                      edc_channel_adapter);
    struct edc_task_group *group = edc_task_group_new(t);
    edc_session_add_group(session, group);
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "k", "v");
    struct edc_tool_call_record *rec = edc_task_tool_call_add(t, "call-1", "echo", args);
    CHECK(rec != NULL, "anti-vacuous: a tool call was recorded");
    cJSON_Delete(args);
    /* Flush the tree to the DB so a load sees it. */
    CHECK(edc_state_store_flush(session, NULL), "state flushed with the tool call");

    int64_t sid = session->session_id;
    g_hooks_on = 1;
    g_live = g_allocs = 0;
    int loads_ok = 0;
    for (int i = 0; i < 20; i++)
    {
        struct edc_session *loaded = edc_session_new(cfg, sid, "Loaded");
        if (edc_state_store_load(loaded, NULL))
            loads_ok++;
        edc_session_free(loaded);
    }
    long leaked = g_live;
    g_hooks_on = 0;
    CHECK(loads_ok > 0, "anti-vacuous: at least one load succeeded");
    CHECK(g_allocs > 0, "anti-vacuous: loads allocate cJSON nodes");
    CHECK(leaked == 0, "20 ledger loads leave zero live cJSON nodes (no parsed-args leak)");

    edc_session_free(session);
    edc_config_free(cfg);
}

int main(void)
{
    /* Install counting hooks once, before anything allocates cJSON. */
    cJSON_Hooks hooks;
    hooks.malloc_fn = counting_malloc;
    hooks.free_fn = counting_free;
    cJSON_InitHooks(&hooks);
    edc_test_workspace();
    test_snapshot_no_leak();
    test_resume_ledger_no_leak();
    fprintf(stderr, "=== %d passed, %d failed ===\n", g_pass, g_fail);
    edc_test_store_down(NULL);
    return g_fail ? 1 : 0;
}
