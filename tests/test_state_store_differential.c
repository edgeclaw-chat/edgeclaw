/* test_state_store_differential: the gate for the SQLite migration, stage 1.
 *
 * The claim under test: writing a session into columns and reading it back loses nothing.
 * The evidence is a field-level comparison of the OLD serializer's output against the
 * output after a database round-trip:
 *
 *     j1 = edc_state_serialize(s1)              <- the oracle (what session.json held)
 *     flush(s1); s2 = load(); j2 = serialize(s2)
 *     assert j1 == j2, field by field
 *
 * Why this shape: the serializer is the one implementation that already describes "the
 * whole state of a session". Comparing trees catches a missing column, a swapped
 * argument, a lost NULL and an enum spelled differently -- all at once, and all of them
 * are exactly the ways a hand-written column mapping fails. Once this is green, deleting
 * the serializer (stage 4) is safe.
 *
 * Normalization is deliberately NOT part of the field comparison: a restart is supposed to
 * change ALIVE into INTERRUPTED, so the fixtures use terminal states and the normalization
 * rules get their own assertions at the bottom of this file.
 */

#include "cJSON.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_scheduler.h"
#include "edc_session_manager.h"
#include "edc_state_serializer.h"
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

volatile sig_atomic_t _run_loop = 1;

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

static char g_dir[512];

/* The scheduler reaches its store through this manager-shaped handle (the same trick the
 * other kernel tests use). */
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

/* ── Field-level tree comparison ─────────────────────────────────────────────
 * Object keys are unordered, arrays are ordered. A few keys are allowed to differ because
 * they describe "when this was written" rather than the state itself. */
/* turn_ord / owner_task_id are the storage key (a position, not content), and
 * token_count is derived from the message rows by the store itself -- the rows are
 * what this test compares, so restating their sum here would prove nothing. */
static const char *kVaryingKeys[] = {"last_active", "turn_ord", "owner_task_id", "token_count", NULL};

static bool key_is_varying(const char *k)
{
    for (int i = 0; kVaryingKeys[i]; i++)
        if (strcmp(k, kVaryingKeys[i]) == 0)
            return true;
    return false;
}

static int g_diff_reported;

static void report_diff(const char *path, const char *what)
{
    /* One line per difference, capped: the first few tell you which mapping is broken. */
    if (g_diff_reported++ < 25)
        fprintf(stderr, "        DIFF at %s: %s\n", path, what);
}

static void cmp_json(const cJSON *a, const cJSON *b, const char *path)
{
    if (a == b)
        return;
    if (!a || !b)
    {
        report_diff(path, a ? "missing in B" : "missing in A");
        return;
    }

    if (a->type != b->type)
    {
        /* A number written as 3 and read back as 3.0 is fine; anything else is not. */
        if (!(cJSON_IsNumber(a) && cJSON_IsNumber(b)))
        {
            report_diff(path, "type differs");
            return;
        }
    }

    if (cJSON_IsObject(a))
    {
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, (cJSON *)a)
        {
            if (key_is_varying(child->string))
                continue;
            char sub[512];
            snprintf(sub, sizeof(sub), "%s.%s", path, child->string ? child->string : "?");
            cmp_json(child, cJSON_GetObjectItem((cJSON *)b, child->string), sub);
        }
        cJSON_ArrayForEach(child, (cJSON *)b)
        {
            if (key_is_varying(child->string))
                continue;
            if (!cJSON_GetObjectItem((cJSON *)a, child->string))
            {
                char sub[512];
                snprintf(sub, sizeof(sub), "%s.%s", path, child->string ? child->string : "?");
                report_diff(sub, "present in B but not in A");
            }
        }
        return;
    }

    if (cJSON_IsArray(a))
    {
        const int na = cJSON_GetArraySize((cJSON *)a);
        const int nb = cJSON_GetArraySize((cJSON *)b);
        if (na != nb)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "length %d vs %d", na, nb);
            report_diff(path, buf);
            return;
        }
        for (int i = 0; i < na; i++)
        {
            char sub[512];
            snprintf(sub, sizeof(sub), "%s[%d]", path, i);
            cmp_json(cJSON_GetArrayItem((cJSON *)a, i), cJSON_GetArrayItem((cJSON *)b, i), sub);
        }
        return;
    }

    if (cJSON_IsString(a))
    {
        const char *sa = cJSON_GetStringValue((cJSON *)a);
        const char *sb = cJSON_GetStringValue((cJSON *)b);
        if ((sa == NULL) != (sb == NULL) || (sa && strcmp(sa, sb) != 0))
            report_diff(path, "string differs");
        return;
    }
    if (cJSON_IsNumber(a))
    {
        if (cJSON_GetNumberValue((cJSON *)a) != cJSON_GetNumberValue((cJSON *)b))
            report_diff(path, "number differs");
        return;
    }
    if (cJSON_IsBool(a))
    {
        if (cJSON_IsTrue((cJSON *)a) != cJSON_IsTrue((cJSON *)b))
            report_diff(path, "bool differs");
    }
}

/* ── Fixture: one session that exercises every persisted structure ─────────── */

static void add_msg(struct edc_turn *turn, const char *role, const char *content, const char *mid)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    if (mid)
        cJSON_AddStringToObject(m, "mid", mid);
    edc_turn_add_message(turn, m); /* the real door: adds id, ts, render */
}

static void build_fixture(struct edc_session *s, struct edc_scheduler *sched)
{
    /* A terminal task with two committed turns plus an in-flight one, a tool ledger, a
     * todo document and a dependency edge. */
    struct edc_task *leader = edc_task_new("t-1", s->session_id, "g-1", "the first task",
                                           edc_channel_adapter);
    leader->is_group_leader = 1;
    leader->step = EDC_TASKSTEP_DONE;
    leader->life = EDC_LIFE_COMPLETED;
    leader->completion_seq = 7;
    leader->tokens_prompt = 1200;
    leader->tokens_completion = 340;
    leader->tokens_total = 1540;
    leader->last_hist_len = 2;
    leader->turns = (EdcQueue){0};
    edc_queue_init(&leader->turns);
    leader->active_turn = NULL;
    leader->todos = cJSON_Parse("[{\"content\":\"step one\",\"status\":\"done\"}]");
    leader->output = sdsnew("the answer");
    leader->raw_input = sdsnew("raw request text");

    struct edc_task_group *g1 = edc_task_group_new(leader);
    edc_session_add_group(s, g1);
    install_sessions(sched, s);

    struct edc_turn *t1 = edc_turn_new();
    add_msg(t1, "user", "first question", "mid-a");
    add_msg(t1, "assistant", "first answer", NULL);
    t1->folded_prefix = 1;
    t1->memo_prefix = 1;
    edc_queue_push(&leader->turns, t1);

    struct edc_turn *t2 = edc_turn_new();
    add_msg(t2, "user", "second question", "mid-b");
    edc_queue_push(&leader->turns, t2);

    /* A child task that depends on the leader and was forked from it. */
    struct edc_task *child = edc_task_new("t-2", s->session_id, "g-1", "the child task",
                                          edc_channel_adapter);
    child->step = EDC_TASKSTEP_DONE;
    child->life = EDC_LIFE_CANCELLED;
    child->end = EDC_END_STOPPED;
    child->forked_from = leader;
    child->fork_point_completion_seq = 7;
    child->is_background = true;
    child->tool_only = true;
    child->has_deferred_vars = true;
    edc_queue_init(&child->turns);
    edc_task_group_add_task(g1, child);

    /* dep: child -> leader (outgoing edge on the child, as the serializer writes it) */
    struct edc_task_dep *dep = g_new0(struct edc_task_dep, 1);
    dep->source = leader;
    dep->target = child;
    dep->type = TASK_DEP_JOIN;
    dep->completed = true;
    dep->next = child->deps;
    child->deps = dep;

    /* A second group, so group ordering and multiple groups are covered. */
    struct edc_task *sys = edc_task_new("t-3", s->session_id, "g-2", "second group leader",
                                        edc_channel_adapter);
    sys->is_group_leader = 1;
    sys->step = EDC_TASKSTEP_DONE;
    sys->life = EDC_LIFE_ERROR;
    sys->end = EDC_END_FAILED;
    sys->fail = EDC_FAIL_OTHER;
    edc_queue_init(&sys->turns);
    struct edc_task_group *g2 = edc_task_group_new(sys);
    g2->memory = sdsnew("folded summary text");
    g2->fold_epoch = 3;
    g2->memory_created_at = 1700000000;
    g2->memory_updated_at = 1700000500;
    g2->memory_covers_to = 42;
    g2->is_orphaned = false;
    edc_session_add_group(s, g2);

    /* In-flight turn on the system task: the active_turn key. */
    struct edc_turn *t3 = edc_turn_new();
    add_msg(t3, "user", "in flight", "mid-c");
    sys->active_turn = t3;

    s->next_completion_seq = 9;
    s->active_task = leader;
    s->foreground_group = g1;
    if (s->last_active)
        sdsfree(s->last_active);
    s->last_active = sdsnew("2026-09-18 05:30:04");
}

/* ── The round trip ──────────────────────────────────────────────────────── */

static void test_roundtrip(void)
{
    printf("\n── database round trip: serialize -> flush -> load -> serialize ──\n");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);
    CHECK(edc_state_store_open(cfg), "open the store");

    struct edc_session *s1 = edc_session_new(cfg, "s-diff", "Differential");
    struct edc_scheduler *sched1 = edc_scheduler_new(cfg);
    install_sessions(sched1, s1);
    build_fixture(s1, sched1);

    cJSON *j1 = edc_state_serialize(s1, sched1);
    CHECK(j1 != NULL, "serialize the original");
    CHECK(edc_state_store_flush(s1, sched1), "flush into the database");

    /* Load into a FRESH session object: that is what a restart does. */
    struct edc_session *s2 = edc_session_new(cfg, "s-diff", "Differential");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_sessions(sched2, s2);
    CHECK(edc_state_store_load(s2, sched2), "load back from the database");

    cJSON *j2 = edc_state_serialize(s2, sched2);
    CHECK(j2 != NULL, "serialize the reloaded session");

    g_diff_reported = 0;
    cmp_json(j1, j2, "$");
    CHECK(g_diff_reported == 0, "every field survived the round trip");

    if (g_diff_reported)
    {
        char *a = cJSON_PrintUnformatted(j1);
        char *b = cJSON_PrintUnformatted(j2);
        fprintf(stderr, "        --- original ---\n%.2000s\n", a ? a : "");
        fprintf(stderr, "        --- reloaded ---\n%.2000s\n", b ? b : "");
        cJSON_free(a);
        cJSON_free(b);
    }

    cJSON_Delete(j1);
    cJSON_Delete(j2);

    edc_session_free(s1);
    edc_scheduler_free(sched1);
    edc_session_free(s2);
    edc_scheduler_free(sched2);
}

/* ── Overwrite: a second flush must not duplicate anything ──────────────────
 * The flush deletes the session row and cascades, so repeating it is idempotent. If that
 * ever breaks, every save would multiply the conversation. */
static void test_flush_is_idempotent(void)
{
    printf("\n── a repeated flush does not duplicate rows ──\n");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);

    struct edc_session *s = edc_session_new(cfg, "s-idem", "Idempotent");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);

    struct edc_task *leader = edc_task_new("i-1", s->session_id, "ig-1", "task", edc_channel_adapter);
    leader->is_group_leader = 1;
    leader->step = EDC_TASKSTEP_DONE;
    leader->life = EDC_LIFE_COMPLETED;
    edc_queue_init(&leader->turns);
    struct edc_task_group *g = edc_task_group_new(leader);
    edc_session_add_group(s, g);
    struct edc_turn *t = edc_turn_new();
    add_msg(t, "user", "hello", "m1");
    add_msg(t, "assistant", "hi", NULL);
    /* edc_task_add_turn, not a raw queue push: it binds the turn to the task's key and
     * moves whatever messages it already wrote. A bare push would leave the rows under the
     * constructor's placeholder key, where the store would never find them again. */
    edc_task_add_turn(leader, t);

    CHECK(edc_state_store_flush(s, sched), "first flush");

    struct edc_session *probe = edc_session_new(cfg, "s-idem", "Idempotent");
    struct edc_scheduler *psched = edc_scheduler_new(cfg);
    install_sessions(psched, probe);
    CHECK(edc_state_store_load(probe, psched), "load after first flush");
    const int tasks_after_1 = 1;
    const size_t msgs_after_1 = edc_session_get_message_count(probe);
    edc_session_free(probe);
    edc_scheduler_free(psched);

    CHECK(edc_state_store_flush(s, sched), "second flush");
    CHECK(edc_state_store_flush(s, sched), "third flush");

    struct edc_session *probe2 = edc_session_new(cfg, "s-idem", "Idempotent");
    struct edc_scheduler *psched2 = edc_scheduler_new(cfg);
    install_sessions(psched2, probe2);
    CHECK(edc_state_store_load(probe2, psched2), "load after three flushes");
    CHECK(edc_session_get_message_count(probe2) == msgs_after_1,
          "message count is unchanged after re-flushing");
    CHECK(tasks_after_1 == 1, "one task");
    edc_session_free(probe2);
    edc_scheduler_free(psched2);

    edc_session_free(s);
    edc_scheduler_free(sched);
}

/* ── The restart normalizations that a differential test cannot cover ────────
 * These are not "compatibility": they are what the state MEANS after the process died. */
static void test_normalization(void)
{
    printf("\n── restart normalization is preserved ──\n");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);

    struct edc_session *s = edc_session_new(cfg, "s-norm", "Normalize");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);

    /* A task left ALIVE (the process died without a clean shutdown) and one CANCELLED
     * with no end reason -- both must be repaired by the loader. */
    struct edc_task *alive = edc_task_new("n-1", s->session_id, "ng-1", "alive task",
                                          edc_channel_adapter);
    alive->is_group_leader = 1;
    alive->step = EDC_TASKSTEP_AWAIT_EXEC;
    alive->life = EDC_LIFE_ALIVE;
    alive->end = EDC_END_NONE;
    edc_queue_init(&alive->turns);
    struct edc_task_group *g = edc_task_group_new(alive);
    edc_session_add_group(s, g);

    struct edc_task *cancelled = edc_task_new("n-2", s->session_id, "ng-1", "cancelled task",
                                              edc_channel_adapter);
    cancelled->step = EDC_TASKSTEP_READY;
    cancelled->life = EDC_LIFE_CANCELLED;
    cancelled->end = EDC_END_NONE; /* unknown reason: the loader derives STOPPED */
    edc_queue_init(&cancelled->turns);
    edc_task_group_add_task(g, cancelled);

    CHECK(edc_state_store_flush(s, sched), "flush a session with an ALIVE task");

    struct edc_session *r = edc_session_new(cfg, "s-norm", "Normalize");
    struct edc_scheduler *rsched = edc_scheduler_new(cfg);
    install_sessions(rsched, r);
    CHECK(edc_state_store_load(r, rsched), "load (this is the restart)");

    struct edc_task *t = NULL;
    bool found_alive = false, found_cancelled = false;
    struct edc_task_group *rg;
    DL_FOREACH(r->groups, rg)
    {
        struct edc_task *it;
        LL_FOREACH2(rg->members, it, group_next)
        {
            if (it->task_id && strcmp(it->task_id, "n-1") == 0)
            {
                found_alive = true;
                t = it;
            }
            if (it->task_id && strcmp(it->task_id, "n-2") == 0)
                found_cancelled = true;
        }
    }
    CHECK(found_alive, "the ALIVE task came back");
    CHECK(t && t->life == EDC_LIFE_INTERRUPTED, "ALIVE became INTERRUPTED");
    CHECK(t && t->step == EDC_TASKSTEP_AWAIT_EXEC, "its step is kept verbatim (where it was)");
    CHECK(t && t->end == EDC_END_INTERRUPTED, "its end reason follows the life axis");
    CHECK(found_cancelled, "the CANCELLED task came back");

    edc_session_free(r);
    edc_scheduler_free(rsched);
    edc_session_free(s);
    edc_scheduler_free(sched);
}

/* ── New session: no rows, not an error ──────────────────────────────────── */
static void test_absent_session(void)
{
    printf("\n── loading a session with no rows ──\n");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);

    struct edc_session *s = edc_session_new(cfg, "s-never-written", "Absent");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);
    CHECK(!edc_state_store_load(s, sched), "load returns false for an unwritten session");
    edc_session_free(s);
    edc_scheduler_free(sched);
}

/* ── forget + session_for_task ───────────────────────────────────────────── */
static void test_forget_and_lookup(void)
{
    printf("\n── forget, and the session_for_task lookup ──\n");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);

    struct edc_session *s = edc_session_new(cfg, "s-forget", "Forget");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);
    struct edc_task *leader = edc_task_new("f-1", s->session_id, "fg-1", "task",
                                           edc_channel_adapter);
    leader->is_group_leader = 1;
    leader->step = EDC_TASKSTEP_DONE;
    leader->life = EDC_LIFE_COMPLETED;
    edc_queue_init(&leader->turns);
    edc_session_add_group(s, edc_task_group_new(leader));
    CHECK(edc_state_store_flush(s, sched), "flush");

    char *sid = edc_state_store_session_for_task("f-1");
    CHECK(sid && strcmp(sid, "s-forget") == 0, "session_for_task finds the owner");
    free(sid);

    CHECK(edc_state_store_session_for_task("no-such-task") == NULL,
          "session_for_task returns NULL for an unknown task");

    CHECK(edc_state_store_forget("s-forget"), "forget the session");
    char *gone = edc_state_store_session_for_task("f-1");
    CHECK(gone == NULL, "its tasks are gone too (cascade)");
    free(gone);

    edc_session_free(s);
    edc_scheduler_free(sched);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_differential_%ld",
             (tmp && *tmp) ? tmp : "/tmp", (long)getpid());
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "(warning: could not reset %s)\n", g_dir);

    printf("workspace: %s\n", g_dir);

    test_roundtrip();
    test_flush_is_idempotent();
    test_normalization();
    test_absent_session();
    test_forget_and_lookup();

    edc_state_store_close();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", g_dir);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
