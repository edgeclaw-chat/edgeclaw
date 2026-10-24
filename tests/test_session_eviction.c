/* Session eviction: the in-memory tree is a cache of the database, so it can be given
 * back -- but only when that is provably safe.
 *
 * What this pins:
 *   1. an unloaded session is a shell: state_loaded=false and its groups are gone;
 *   2. the ACTIVE session is never unloaded (the scheduler is driving it);
 *   3. a session with unfinished work is never unloaded (a background task would lose
 *      everything it holds in memory);
 *   4. the data survives: reloading after an unload gives back the tasks and messages;
 *   5. eviction honours the cap while still keeping the active session.
 */
#include "edc_compat.h"
#include "edc_log.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_session_store.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "test_ws.h"
#include "utlist_fix.h"

#include <stdio.h>
#include <string.h>

static int g_failed;
static int g_passed;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

static void install_sessions(struct edc_scheduler *sched, struct edc_session_manager *sm)
{
    sched->sessions = sm;
    if (sm->session_count > 0)
        sched->ctx = sm->sessions[0];
}

/* One committed turn on a fresh task in its own group. */
static void add_work(struct edc_session *s, struct edc_scheduler *sched, const char *task_id,
                     const char *text)
{
    struct edc_task *t = edc_task_new(task_id, s->session_id, task_id, text, edc_channel_adapter);
    edc_task_group_add_task(edc_task_group_new(t), t);
    struct edc_task_group *g = t->group;
    edc_session_add_group(s, g);
    s->active_task = t;

    struct edc_turn *turn = edc_turn_new_for(s->session_id, g->group_id, t->task_id, 0);
    edc_turn_add_user_message(turn, text);
    edc_task_add_turn(t, turn);
    edc_task_group_history_add_turn(g, turn);

    edc_task_await_exec(t);
    edc_task_mark_completed(t);
    edc_task_set_completion_seq(t, s->next_completion_seq++);
    (void)sched;
}

/* How many messages this session's groups can show (through the slot API). */
static int session_history_count(struct edc_session *s)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(s->groups, g) n += edc_task_group_history_count(g);
    return n;
}

static int session_task_count(struct edc_session *s)
{
    int n = 0;
    struct edc_task_group *g;
    DL_FOREACH(s->groups, g)
    {
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next) n++;
    }
    return n;
}

int main(void)
{
    edc_test_workspace();
    edc_log_init();

    struct edc_config *cfg = edc_test_config();
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, sm);

    /* ── 1. a session with work can be unloaded, and becomes a shell ── */
    printf("\n── unload turns a session back into a shell ──\n");
    struct edc_session *a = edc_session_manager_get_active(sm);
    add_work(a, sched, "ta-1", "hello from A");
    a->state_loaded = true;
    CHECK(session_task_count(a) == 1, "A holds its task in memory");
    CHECK(session_history_count(a) == 1, "A's group shows its message");

    /* Make a second session active, so A is a normal candidate. */
    struct edc_session *b = edc_session_manager_create(sm, "B");
    b->state_loaded = true;
    CHECK(edc_session_manager_get_active(sm) == b, "B is active now");

    CHECK(edc_session_unload(sm, sched, a), "A unloads");
    CHECK(a->state_loaded == false, "A is a shell again (state_loaded=false)");
    CHECK(a->groups == NULL, "A's tree is released");
    CHECK(edc_session_get_name(a) != NULL, "A keeps its identity (name/created_at)");

    /* ── 2. the active session is never unloaded ── */
    printf("\n── the active session is never unloaded ──\n");
    CHECK(!edc_session_unload(sm, sched, b), "refuses the active session");
    CHECK(b->state_loaded == true, "B is untouched");

    /* ── 3. unfinished work pins a session ── */
    printf("\n── a session with live work is pinned ──\n");
    struct edc_session *c = edc_session_manager_create(sm, "C");
    c->state_loaded = true;
    {
        struct edc_task *t = edc_task_new("tc-1", c->session_id, "tc-1", "running", edc_channel_adapter);
        edc_task_group_add_task(edc_task_group_new(t), t);
        edc_session_add_group(c, t->group);
        edc_task_await_exec(t); /* ALIVE + AWAIT_EXEC: may still advance by itself */
        CHECK(edc_task_is_alive(t), "C holds an ALIVE task");
    }
    CHECK(edc_session_has_live_work(c), "has_live_work sees it");
    CHECK(!edc_session_unload(sm, sched, c), "refuses a session with live work");
    CHECK(c->state_loaded == true, "C keeps its tree");

    /* Finish it, and it becomes evictable. */
    {
        struct edc_task *t = edc_session_manager_find_task_any(sm, "tc-1", NULL);
        CHECK(t != NULL, "the live task is findable");
        if (t)
            edc_task_mark_completed(t);
    }
    CHECK(!edc_session_has_live_work(c), "no live work once the task completed");

    /* ── 4. the data survives the round trip ── */
    printf("\n── reload after unload gives the work back ──\n");
    CHECK(!a->state_loaded, "A is still a shell");
    CHECK(edc_session_store_load_session(sm, sched, a), "A reloads");
    CHECK(a->state_loaded, "A is loaded again");
    CHECK(session_task_count(a) == 1, "A's task is back");
    /* The count is NOT 1 here and should not be: the history is DERIVED, and a reload
     * rebuilds it from the turns -- which re-appends the system prompt in slot 0 that this
     * test never put there when it built the session by hand. What matters is that the
     * message itself came back, in the right slot, with the right content. */
    CHECK(session_history_count(a) == 2, "A's history rebuilt: system prompt + the message");
    {
        struct edc_task_group *g = a->groups;
        cJSON *sys = g ? edc_task_group_history_at(g, 0) : NULL;
        cJSON *msg = g ? edc_task_group_history_at(g, 1) : NULL;
        CHECK(sys && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sys, "role")), "system") == 0,
              "slot 0 is the system prompt again");
        CHECK(msg && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content")),
                            "hello from A") == 0,
              "A's message is back, with its content");
        if (sys)
            cJSON_Delete(sys);
        if (msg)
            cJSON_Delete(msg);
    }
    {
        struct edc_task *t = edc_session_manager_find_task_any(sm, "ta-1", NULL);
        CHECK(t != NULL, "the task is findable by id");
        CHECK(t != NULL && edc_task_is_completed(t), "and keeps its completed state");
    }

    /* ── 5. eviction honours the cap and keeps the active session ── */
    printf("\n── eviction keeps the cap ──\n");
    {
        const int before = edc_session_manager_loaded_count(sm);
        CHECK(before >= 3, "three sessions are resident");
        int dropped = edc_session_manager_evict_idle(sm, sched, 2);
        CHECK(dropped >= 1, "evicted at least one");
        CHECK(edc_session_manager_loaded_count(sm) <= 2, "at most 2 remain resident");
        CHECK(b->state_loaded, "the active session is still resident");
    }

    /* Evicting again with the same cap is a no-op: nothing left that may go. */
    {
        const int n = edc_session_manager_loaded_count(sm);
        CHECK(edc_session_manager_evict_idle(sm, sched, 2) == 0, "second pass evicts nothing");
        CHECK(edc_session_manager_loaded_count(sm) == n, "count unchanged");
    }

    /* ── 5b. a cross-session dependency pins the source's session ── */
    printf("\n── a session another session depends on is pinned ──\n");
    {
        /* A task in D depends on a task in A. `/task new -d <id>` allows this on purpose
         * (ids are globally unique), and the dependent holds a LIVE pointer to the source,
         * so freeing A's tree would leave it dereferencing freed memory. */
        struct edc_session *d = edc_session_manager_create(sm, "D");
        d->state_loaded = true;
        /* Step 5 may have evicted A already; bring it back so there is a task to point at. */
        if (!a->state_loaded)
            CHECK(edc_session_store_load_session(sm, sched, a), "A reloads for the dep test");
        struct edc_task *src = edc_session_manager_find_task_any(sm, "ta-1", NULL);
        struct edc_task *dep_task =
            edc_task_new("td-1", d->session_id, "td-1", "depends on A", edc_channel_adapter);
        edc_task_group_add_task(edc_task_group_new(dep_task), dep_task);
        edc_session_add_group(d, dep_task->group);
        CHECK(src != NULL, "A's task is loaded and findable");
        if (src)
        {
            CHECK(edc_task_add_dependency(dep_task, src, TASK_DEP_JOIN),
                  "the cross-session edge is accepted");
            edc_task_await_exec(dep_task); /* queued: it has not injected src->output yet */
        }

        /* Make D active so A is a plain candidate again. */
        sm->active_index = sm->session_count - 1;
        CHECK(edc_session_manager_get_active(sm) == d, "D is active");

        CHECK(!edc_session_unload(sm, sched, a), "refuses: another session depends on A's task");
        CHECK(a->state_loaded, "A keeps its tree");
        if (src)
        {
            struct edc_task_dep *dep = dep_task->deps;
            CHECK(dep != NULL && dep->source == src, "the dependent still points at the live task");
        }

        /* Once the dependent is gone the edge goes with it, and A becomes evictable. */
        edc_task_group_remove_task(dep_task->group, dep_task);
        edc_task_free(dep_task);
        d->groups = NULL; /* the group was emptied by the removal */
        CHECK(edc_session_unload(sm, sched, a), "A unloads once nothing points into it");
    }

    /* Resident right now: b (active), plus whichever of a/c survived step 5. */
    {
        int dropped = edc_session_manager_evict_idle(sm, sched, 2);
        (void)dropped;
        CHECK(edc_session_manager_loaded_count(sm) <= 2, "still within the cap");
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    edc_test_store_down(cfg);
    return g_failed == 0 ? 0 : 1;
}
