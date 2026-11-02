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

#include "sqlite3.h"
#include "cJSON.h"
#include "test_ws.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_scheduler.h"
#include "edc_session_manager.h"
#include "edc_state_store.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "log.h"
#include "sds.h"
#include "utlist_fix.h"

#include <inttypes.h> /* PRId64 */
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

/* The oracle is the DATABASE, because it is the only representation left.
 *
 * This test used to serialize the live tree and the reloaded tree and compare the two
 * documents; with no document, "did every field survive" is answered by dumping the session's
 * rows before and after the round trip. Two columns are excluded on purpose: `updated_at` is
 * "now" by definition, and `last_active` moves when a session is touched -- neither is state
 * the loader is supposed to restore.
 *
 * Rows are sorted so the comparison is about CONTENT. Ordering is covered separately by the
 * ord-bearing tables, whose values are part of the dump. */
static const char *const kDumpTables[] = {
    "sessions", "groups", "tasks", "turns", "messages",
    "task_deps", "task_tool_calls", "memo_entries", "memo_events", "group_fold_files",
};

static int cmp_sds_line(const void *a, const void *b);

static bool dump_skip_column(const char *name)
{
    return strcmp(name, "updated_at") == 0 || strcmp(name, "last_active") == 0;
}

/* Everything the dump reads, as text. Read through the store's own handle: the store holds
 * the database exclusively, so the second connection this used to open would only get
 * SQLITE_BUSY. The comparison is still a comparison of ROWS -- SQLite's, not the in-memory
 * tree's -- which is what makes it a check on the mapping rather than a tautology. */
static sds dump_rows(const char *session_id)
{
    sqlite3 *db = edc_state_store_handle();
    if (!db)
        return NULL;

    sds out = sdsempty();
    for (size_t t = 0; t < sizeof(kDumpTables) / sizeof(kDumpTables[0]); t++)
    {
        const char *table = kDumpTables[t];

        /* Which columns exist, and which of them to skip. */
        sds cols = sdsempty();
        int kept = 0;
        {
            sds sql = sdscatprintf(sdsempty(), "PRAGMA table_info(%s)", table);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK)
            {
                while (sqlite3_step(st) == SQLITE_ROW)
                {
                    const char *name = (const char *)sqlite3_column_text(st, 1);
                    if (!name || dump_skip_column(name))
                        continue;
                    if (kept++)
                        cols = sdscat(cols, ",");
                    cols = sdscat(cols, name);
                }
            }
            sqlite3_finalize(st);
            sdsfree(sql);
        }

        if (kept > 0)
        {
            sds sql = sdscatprintf(sdsempty(), "SELECT %s FROM %s", cols, table);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK)
            {
                /* Collect the rows first so they can be sorted: a dump compared line by line
                 * must not depend on the order the query happened to return. */
                sds *lines = NULL;
                size_t n = 0, cap = 0;
                while (sqlite3_step(st) == SQLITE_ROW)
                {
                    sds line = sdscatprintf(sdsempty(), "%s|", table);
                    for (int i = 0; i < kept; i++)
                    {
                        if (i)
                            line = sdscat(line, "|");
                        if (sqlite3_column_type(st, i) == SQLITE_NULL)
                            line = sdscat(line, "\x01NULL");
                        else
                        {
                            const unsigned char *txt = sqlite3_column_text(st, i);
                            line = sdscat(line, txt ? (const char *)txt : "");
                        }
                    }
                    if (n == cap)
                    {
                        cap = cap ? cap * 2 : 64;
                        sds *grown = realloc(lines, cap * sizeof(*grown));
                        if (!grown)
                            break;
                        lines = grown;
                    }
                    lines[n++] = line;
                }
                if (n > 1)
                    qsort(lines, n, sizeof(*lines), cmp_sds_line);
                for (size_t i = 0; i < n; i++)
                {
                    out = sdscat(out, lines[i]);
                    out = sdscat(out, "\n");
                    sdsfree(lines[i]);
                }
                free(lines);
            }
            sqlite3_finalize(st);
            sdsfree(sql);
        }
        sdsfree(cols);
    }
    return out;
}

static int cmp_sds_line(const void *a, const void *b)
{
    const sds *x = a;
    const sds *y = b;
    return strcmp(*x, *y);
}

/* Report the first differences between two dumps, one line each. */
static void report_dump_diff(const sds before, const sds after)
{
    int reported = 0;
    char *tmp_a = sdsdup(before ? before : sdsempty());
    char *tmp_b = sdsdup(after ? after : sdsempty());
    char *save_a = NULL, *save_b = NULL;
    for (char *la = strtok_r(tmp_a, "\n", &save_a), *lb = strtok_r(tmp_b, "\n", &save_b);
         (la || lb) && reported < 25;
         la = strtok_r(NULL, "\n", &save_a), lb = strtok_r(NULL, "\n", &save_b))
    {
        if (la && lb && strcmp(la, lb) == 0)
            continue;
        if (!la || !lb || strcmp(la, lb) != 0)
        {
            fprintf(stderr, "        DIFF before: %.200s\n", la ? la : "(end)");
            fprintf(stderr, "        DIFF after : %.200s\n", lb ? lb : "(end)");
            reported++;
        }
    }
    sdsfree(tmp_a);
    sdsfree(tmp_b);
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
    struct edc_task *leader = edc_task_new(edc_test_id("t-1"), s->session_id, edc_test_id("g-1"), "the first task",
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
    /* edc_task_add_turn, not a raw push: it binds the turn to the task's storage key and
     * moves the messages it already wrote. A bare push leaves them under the helper's
     * placeholder id, where the store never finds them again. */
    edc_task_add_turn(leader, t1);

    struct edc_turn *t2 = edc_turn_new();
    add_msg(t2, "user", "second question", "mid-b");
    edc_task_add_turn(leader, t2);

    /* A child task that depends on the leader and was forked from it. */
    struct edc_task *child = edc_task_new(edc_test_id("t-2"), s->session_id, edc_test_id("g-1"), "the child task",
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

    /* dep: child -> leader. Built through the real door, NOT by hand: an edge is a PAIR of
     * nodes (child->deps and leader->dependents) and only edc_task_add_dependency keeps them
     * in step. A hand-built one-sided edge used to live here, and it was a use-after-free
     * waiting for a teardown order: freeing the leader (which has no matching dependents
     * node) left the child's dep pointing at freed memory, and freeing the child then read
     * `dep->source->dependents`. leader is already COMPLETED above, so the API marks the
     * edge completed itself -- exactly the state this fixture is after. */
    edc_task_add_dependency(child, leader, TASK_DEP_JOIN);

    /* A second group, so group ordering and multiple groups are covered. */
    struct edc_task *sys = edc_task_new(edc_test_id("t-3"), s->session_id, edc_test_id("g-2"), "second group leader",
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

    /* In-flight turn on the system task: the active_turn key.
     *
     * Bound to the task's REAL key at creation. `edc_turn_new()` is the test helper, which
     * keys the turn on a placeholder id; the messages it writes then live under that id, and
     * a turn assigned to active_turn afterwards is never re-keyed (nothing calls
     * edc_task_add_turn on it), so it would claim a message the store cannot find -- a
     * msg_count with no rows behind it. The dump-based oracle below is what caught it. */
    struct edc_turn *t3 = edc_turn_new_for(s->session_id, sys->group_id, sys->task_id, 0);
    add_msg(t3, "user", "in flight", "mid-c");
    sys->active_turn = t3;

    s->next_completion_seq = 9;
    s->active_task = leader;
    s->foreground_group = g1;
    /* An integer, like every other timestamp now. */
    s->last_active = 1790000000ULL;

    /* v8 columns get NON-DEFAULT values here, otherwise the row-for-row comparison below
     * proves nothing about them: the dump reads every column, so a flush/load mismatch
     * would only show up if the stored value differs from the fresh default. */
    leader->memo_baseline_done = true; /* a task that has already received the catalogue */
    {
        sds id = edc_session_memory_create_entry(s->memo, "round-trip entry", "its summary", "its detail text", NULL);
        const uint64_t seq =
            edc_session_memory_add_event(s->memo, id, EDC_MEMO_UPDATED, "round-trip entry", "its summary");
        struct edc_memo_entry *e = edc_session_memory_entry(s->memo, id);
        if (e)
            e->created_seq = e->updated_seq = seq;
        /* A real mask: 0 is the documented "unknown" value, so leaving the default would
         * let a column that is never written pass unnoticed. */
        s->memo->events[s->memo->event_count - 1].changed = EDC_MEMO_F_SUMMARY | EDC_MEMO_F_DETAIL;
        sdsfree(id);
    }
}

/* ── The round trip ──────────────────────────────────────────────────────── */

static void test_roundtrip(void)
{
    printf("\n── database round trip: serialize -> flush -> load -> serialize ──\n");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);
    CHECK(edc_state_store_open(cfg), "open the store");

    struct edc_session *s1 = edc_session_new(cfg, edc_test_session_id("s-diff"), "Differential");
    struct edc_scheduler *sched1 = edc_scheduler_new(cfg);
    install_sessions(sched1, s1);
    build_fixture(s1, sched1);

    CHECK(edc_state_store_flush(s1, sched1), "flush into the database");

    sds before = dump_rows("s-diff");
    CHECK(before != NULL && sdslen(before) > 0, "dumped the rows after the first flush");

    /* Load into a FRESH session object: that is what a restart does. Then persist it again --
     * a load that lost a field would write the loss back, so the second dump is what proves
     * the round trip carried everything. */
    struct edc_session *s2 = edc_session_new(cfg, edc_test_session_id("s-diff"), "Differential");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    install_sessions(sched2, s2);
    CHECK(edc_state_store_load(s2, sched2), "load back from the database");

    /* ABSOLUTE assertion, not just the differential: the dump comparison proves flush and
     * load AGREE, which a column that is never written (or always written as its default)
     * also satisfies. These check the VALUES came back. */
    {
        bool baseline_ok = false;
        for (struct edc_task_group *gp = s2->groups; gp; gp = gp->next)
            for (struct edc_task *tp = gp->members; tp; tp = tp->group_next)
                if (tp->memo_baseline_done)
                    baseline_ok = true;
        CHECK(baseline_ok, "v8: memo_baseline_done survives a restart");

        bool mask_ok = false;
        for (size_t i = 0; i < s2->memo->event_count; i++)
            if (s2->memo->events[i].changed == (EDC_MEMO_F_SUMMARY | EDC_MEMO_F_DETAIL))
                mask_ok = true;
        CHECK(mask_ok, "v8: the changed mask survives a restart (0 would mean 'unknown')");
    }

    CHECK(edc_state_store_flush(s2, sched2), "flush the reloaded session");

    sds after = dump_rows("s-diff");
    CHECK(after != NULL, "dumped the rows after the round trip");

    const bool same = before && after && strcmp(before, after) == 0;
    if (!same)
        report_dump_diff(before, after);
    CHECK(same, "every row survived the round trip byte for byte");

    sdsfree(before);
    sdsfree(after);

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

    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("s-idem"), "Idempotent");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);

    struct edc_task *leader = edc_task_new(edc_test_id("i-1"), s->session_id, edc_test_id("ig-1"), "task", edc_channel_adapter);
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

    struct edc_session *probe = edc_session_new(cfg, edc_test_session_id("s-idem"), "Idempotent");
    struct edc_scheduler *psched = edc_scheduler_new(cfg);
    install_sessions(psched, probe);
    CHECK(edc_state_store_load(probe, psched), "load after first flush");
    const int tasks_after_1 = 1;
    const size_t msgs_after_1 = edc_session_get_message_count(probe);
    edc_session_free(probe);
    edc_scheduler_free(psched);

    CHECK(edc_state_store_flush(s, sched), "second flush");
    CHECK(edc_state_store_flush(s, sched), "third flush");

    struct edc_session *probe2 = edc_session_new(cfg, edc_test_session_id("s-idem"), "Idempotent");
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

    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("s-norm"), "Normalize");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);

    /* A task left ALIVE (the process died without a clean shutdown) and one CANCELLED
     * with no end reason -- both must be repaired by the loader. */
    struct edc_task *alive = edc_task_new(edc_test_id("n-1"), s->session_id, edc_test_id("ng-1"), "alive task",
                                          edc_channel_adapter);
    alive->is_group_leader = 1;
    alive->step = EDC_TASKSTEP_AWAIT_EXEC;
    alive->life = EDC_LIFE_ALIVE;
    alive->end = EDC_END_NONE;
    edc_queue_init(&alive->turns);
    struct edc_task_group *g = edc_task_group_new(alive);
    edc_session_add_group(s, g);

    struct edc_task *cancelled = edc_task_new(edc_test_id("n-2"), s->session_id, edc_test_id("ng-1"), "cancelled task",
                                              edc_channel_adapter);
    cancelled->step = EDC_TASKSTEP_READY;
    cancelled->life = EDC_LIFE_CANCELLED;
    cancelled->end = EDC_END_NONE; /* unknown reason: the loader derives STOPPED */
    edc_queue_init(&cancelled->turns);
    edc_task_group_add_task(g, cancelled);

    CHECK(edc_state_store_flush(s, sched), "flush a session with an ALIVE task");

    struct edc_session *r = edc_session_new(cfg, edc_test_session_id("s-norm"), "Normalize");
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
            if (it->task_id == edc_test_id("n-1"))
            {
                found_alive = true;
                t = it;
            }
            if (it->task_id == edc_test_id("n-2"))
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

/* ── The memory's removals must reach the database ──────────────────────────
 * `memory_delete` drops an entry FOR REAL and `trim_events` drops old events, but both do it
 * in RAM. The flush used to be insert-only, so a deleted entry came back on the next load --
 * a compaction could delete 30 entries, restart, and find every one of them again. */
static void test_memo_removals_persist(void)
{
    printf("\n── deleted entries and trimmed events stay gone after a reload ──\n");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);

    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("s-memo-del"), "MemoDelete");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);

    /* three entries, each with its CREATED event */
    const char *ids[3] = {NULL, NULL, NULL};
    const char *titles[3] = {"A 标题", "B 标题", "C 标题"};
    for (int i = 0; i < 3; i++)
    {
        const char *id = ids[i] = edc_session_memory_create_entry(s->memo, titles[i], "摘要", "明细正文", "task-1");
        uint64_t seq = edc_session_memory_add_event(s->memo, id, EDC_MEMO_CREATED, titles[i], "摘要");
        struct edc_memo_entry *e = edc_session_memory_entry(s->memo, id);
        if (e)
            e->created_seq = e->updated_seq = seq;
    }
    CHECK(s->memo && s->memo->entry_count == 3, "三个条目");
    CHECK(edc_state_store_flush(s, sched), "第一次 flush（三条都落盘）");

    /* now remove one for real -- that must reach the database, not just RAM */
    const char *drop = ids[1];
    CHECK(edc_session_memory_delete(s->memo, &drop, 1), "真删 m2");
    CHECK(s->memo && s->memo->entry_count == 2, "内存里剩两条");

    /* and let the real trim run, so old events leave RAM too */
    const size_t events_before = s->memo ? s->memo->event_count : 0;
    {
        /* Shrink the log by hand so the trim actually has something to drop: the public trim
         * keeps KEEP_EVENTS, which is more than this fixture ever writes. */
        while (s->memo && s->memo->event_count > 1)
        {
            if (s->memo->events[0].seq > s->memo->pruned_before)
                s->memo->pruned_before = s->memo->events[0].seq;
            memmove(&s->memo->events[0], &s->memo->events[1],
                    (s->memo->event_count - 1) * sizeof(*s->memo->events));
            s->memo->event_count--;
        }
    }
    CHECK(events_before > 1 && s->memo && s->memo->event_count == 1, "内存里只剩最新 1 条事件");
    CHECK(edc_state_store_flush(s, sched), "第二次 flush（删除 + 裁剪都要落盘）");

    /* reload into a fresh object -- what a restart does */
    struct edc_session *back = edc_session_new(cfg, edc_test_session_id("s-memo-del"), "MemoDelete");
    struct edc_scheduler *bsched = edc_scheduler_new(cfg);
    install_sessions(bsched, back);
    CHECK(edc_state_store_load(back, bsched), "重启后 load");

    CHECK(back->memo && back->memo->entry_count == 2, "条目数还是 2（被删的没复活）");
    CHECK(back->memo && edc_session_memory_entry(back->memo, drop) == NULL, "被删的 m2 读不到");
    CHECK(back->memo && edc_session_memory_entry(back->memo, ids[0]) != NULL, "m1 还在");
    CHECK(back->memo && edc_session_memory_entry(back->memo, ids[2]) != NULL, "m3 还在");
    CHECK(back->memo && back->memo->event_count == 1, "事件表也跟着裁剪（旧事件没复活）");
    CHECK(back->memo && back->memo->pruned_before > 0, "pruned_before 跟着回来");

    edc_session_free(back);
    edc_scheduler_free(bsched);
    edc_session_free(s);
    edc_scheduler_free(sched);
    edc_config_free(cfg);
}

/* ── New session: no rows, not an error ──────────────────────────────────── */
static void test_absent_session(void)
{
    printf("\n── loading a session with no rows ──\n");

    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(g_dir);

    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("s-never-written"), "Absent");
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

    struct edc_session *s = edc_session_new(cfg, edc_test_session_id("s-forget"), "Forget");
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    install_sessions(sched, s);
    struct edc_task *leader = edc_task_new(edc_test_id("f-1"), s->session_id, edc_test_id("fg-1"), "task",
                                           edc_channel_adapter);
    leader->is_group_leader = 1;
    leader->step = EDC_TASKSTEP_DONE;
    leader->life = EDC_LIFE_COMPLETED;
    edc_queue_init(&leader->turns);
    edc_session_add_group(s, edc_task_group_new(leader));
    CHECK(edc_state_store_flush(s, sched), "flush");

    char *sid = edc_state_store_session_for_task(edc_test_id("f-1"));
    CHECK(sid && strcmp(sid, edc_id_str(edc_test_session_id("s-forget"))) == 0,
          "session_for_task finds the owner");
    free(sid);

    CHECK(edc_state_store_session_for_task(edc_test_id("no-such-task")) == NULL,
          "session_for_task returns NULL for an unknown task");

    CHECK(edc_state_store_forget(edc_test_session_id("s-forget")), "forget the session");
    char *gone = edc_state_store_session_for_task(edc_test_id("f-1"));
    CHECK(gone == NULL, "its tasks are gone too (cascade)");
    free(gone);

    edc_session_free(s);
    edc_scheduler_free(sched);
}

int main(void)
{
    edc_test_workspace(); /* open the disposable store before anything writes */
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_differential_%" PRId64,
             (tmp && *tmp) ? tmp : "/tmp", (int64_t)getpid());
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "(warning: could not reset %s)\n", g_dir);

    printf("workspace: %s\n", g_dir);

    test_roundtrip();
    test_flush_is_idempotent();
    test_memo_removals_persist();
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
