/* Tests for the scheduler's session-store lifecycle entry points.
 *
 * `edc_scheduler_new` stays infallible and in-memory: 167 call sites (mostly
 * fixtures that install their own store) depend on that. The disk I/O and its
 * failure modes live behind two separate entries instead:
 *
 *   edc_scheduler_open()   assembly time -- build the store, load the active session
 *   edc_scheduler_close()  after the writers stopped -- persist, then drop turns
 *
 * The ownership rule these tests pin: the store is freed by edc_scheduler_free
 * only when open() created it. A fixture installed on `sched->sessions` belongs to
 * its installer (every fixture-based test frees it itself).
 */
#include "edc_config.h"
#include "test_ws.h"
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_session_manager.h"
#include "edc_session.h"
#include "log.h"
#include "sds.h"
#include "sqlite3.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

volatile sig_atomic_t _run_loop = 1;


/* The state used to be one JSON file; it is now rows in <ws>/edgeclaw.db. These two
 * helpers keep the shape of the original assertions ("is there state?", "does the state
 * mention X?") without pretending the file is still there. */
static void state_db_path(char *out, size_t n, const char *ws)
{
    snprintf(out, n, "%s/edgeclaw.db", ws);
}

/* How many task rows the database holds. The database FILE appears as soon as the store
 * is opened, so "has anything been persisted yet" is a question about ROWS, not files. */
static int state_task_rows(const char *ws)
{
    char path[512];
    state_db_path(path, sizeof(path), ws);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        return -1;
    }
    int n = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM tasks", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

/* Every description and every message body, concatenated: enough for a strstr. */
static sds read_state_text(const char *ws)
{
    char path[512];
    state_db_path(path, sizeof(path), ws);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        return NULL;
    }
    sds out = sdsempty();
    const char *queries[] = {"SELECT ifnull(description,'') FROM tasks",
                             /* v3 split the message: `body` holds the wire fields, the
                              * metadata lives in columns. The input text is wire content. */
                             "SELECT body FROM messages", NULL};
    for (int q = 0; queries[q]; q++)
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, queries[q], -1, &st, NULL) == SQLITE_OK)
        {
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                const unsigned char *t = sqlite3_column_text(st, 0);
                if (t)
                {
                    out = sdscat(out, (const char *)t);
                    out = sdscat(out, "\n");
                }
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (sdslen(out) == 0)
    {
        sdsfree(out);
        return NULL;
    }
    return out;
}

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while (0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        log_error("    FAIL: " __VA_ARGS__); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while (0)

static struct edc_config *make_config(const char *workspace)
{
    struct edc_config *c = calloc(1, sizeof(struct edc_config));
    c->workspace = sdsnew(workspace);
    return c;
}

static void free_config(struct edc_config *c)
{
    if (!c)
        return;
    sdsfree(c->workspace);
    free(c);
}

static void make_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/edc_openclose_test_XXXXXX");
    if (!mkdtemp(buf))
    {
        perror("mkdtemp");
        exit(2);
    }
}

/* ── 1. new() is in-memory; open() is the entry that touches disk ─────────
 * Fixtures in ~19 test files set `sched->sessions` themselves and call
 * edc_scheduler_new(cfg) 167 times; this test is what keeps that shape honest. */
static void test_new_is_in_memory_open_is_not(void)
{
    TEST("new() leaves the store unset; open() builds and owns it");
    char ws[256];
    make_tmpdir(ws, sizeof(ws));
    struct edc_config *cfg = make_config(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);

    CHECK(sched->sessions == NULL, "new() does not build a store");
    CHECK(sched->sessions_owned == false, "new() owns nothing");

    CHECK(edc_scheduler_open(sched), "open() succeeds on a fresh workspace");
    CHECK(sched->sessions != NULL, "open() built the store");
    CHECK(sched->sessions_owned == true, "open() marks it owned");

    /* A workspace with no sessions.json gets a usable active session (the store
     * creates a default one), so the first snapshot has something to describe.
     * Read through the manager here (same package): the scheduler no longer
     * exposes an active-session accessor for the frontend to borrow. */
    struct edc_session *active = edc_session_manager_get_active(sched->sessions);
    CHECK(active != NULL && active->session_id > 0,
          "an active session is resolved after open()");

    edc_scheduler_free(sched);
    free_config(cfg);
    rmdir(ws);
}

/* ── 2. close() persists, then is a no-op ────────────────────────────────
 * Observable on disk, not just in return values: the active session's state file
 * must appear, and a second close must not redo the work. */
static void test_close_persists_then_is_idempotent(void)
{
    TEST("close() persists the active session, then refuses a second time");
    char ws[256];
    make_tmpdir(ws, sizeof(ws));
    struct edc_config *cfg = make_config(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    CHECK(edc_scheduler_open(sched), "open()");

    struct edc_session *active = edc_session_manager_get_active(sched->sessions);
    const char *sid = active ? edc_id_str(active->session_id) : NULL;
    CHECK(sid != NULL && *sid, "active session id");

    CHECK(state_task_rows(ws) == 0, "nothing persisted before close()");

    CHECK(edc_scheduler_close(sched), "first close() settles");
    CHECK(state_task_rows(ws) == 0, "close() of an empty session persists no tasks");

    /* Idempotent: the second call reports it did nothing. Re-running the save
     * would be harmless but pointless, and the flag is what makes that visible. */
    CHECK(!edc_scheduler_close(sched), "second close() is a no-op");

    edc_scheduler_free(sched);
    free_config(cfg);
    rmdir(ws);
}

/* ── 2b. close() states what was in flight, and the queue heals ──────────
 * The process going down is written into the state itself (INTERRUPTED, not inferred on
 * the way back in) for every unfinished task -- queued ones included. The queue's order
 * edge is a BARRIER, so a predecessor that will not run on its own releases whoever was
 * queued behind it instead of taking it down. */
static void test_close_interrupts_in_flight_and_heals_the_queue(void)
{
    char ws[256];
    make_tmpdir(ws, sizeof(ws));
    struct edc_config *cfg = make_config(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    CHECK(edc_scheduler_open(sched), "open()");
    struct edc_session *sess = edc_session_manager_get_active(sched->sessions);
    CHECK(sess != NULL, "active session");

    /* A holds the front and is in flight; B joins the group in front, so it queues. */
    struct edc_task *a = edc_scheduler_task_new(sched, sess, "a", edc_channel_adapter);
    struct edc_task_group *g = edc_scheduler_create_group(sched, sess, a);
    edc_scheduler_set_foreground(sess, g);
    edc_scheduler_attach_input(sched, a, "a's input");
    CHECK(edc_task_await_exec(a), "A is in flight (LLM_PROCESSING)");
    a->is_background = true; /* it must keep that across the restart (see below) */

    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.desc = "b";
    spec.input = "b's input";
    spec.place = EDC_TASK_PLACE_FOREGROUND;
    spec.take_foreground = true;
    struct edc_task *b = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(b && edc_task_awaits_deps(b), "B queued behind A while it runs");

    CHECK(edc_scheduler_close(sched), "close() settles");

    TEST("close() interrupts what was in flight, and VOIDS the order edge instead of cascading");
    CHECK(edc_task_is_interrupted(a), "A was in flight -> INTERRUPTED");
    CHECK(b && edc_task_is_interrupted(b), "B was unfinished too, so close() interrupts it as well");
    CHECK(b && b->deps && b->deps->cancelled, "the queue's order edge is voided (BARRIER: the source ended)");
    CHECK(b && b->deps_total == 0, "so it no longer counts as a constraint");
    CHECK(b && edc_task_is_resumable(b), "and B can be resumed...");

    /* Reopen: the state comes back as stated -- nothing is inferred on the way in. */
    int64_t a_id = a->task_id;
    int64_t b_id = b->task_id;
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    CHECK(edc_scheduler_open(sched2), "reopen()");
    struct edc_task *ra = edc_session_manager_find_task_any(sched2->sessions, a_id, 0);
    struct edc_task *rb = edc_session_manager_find_task_any(sched2->sessions, b_id, 0);
    CHECK(ra && edc_task_is_interrupted(ra), "INTERRUPTED round-trips (no inference on load)");
    CHECK(ra && ra->is_background, "and the background flag does too (resuming it must not put it on screen)");
    CHECK(rb && edc_task_is_interrupted(rb), "and B's INTERRUPTED does too");
    CHECK(rb && rb->deps_total == 0, "the voided order edge comes back voided: B is free to run");

    TEST("resuming B does not wait for the stopped task in front: the queue is done with A");
    if (rb)
    {
        edc_scheduler_resume_task(sched2, rb);
        CHECK(!edc_task_awaits_deps(rb), "B did not go back to dep-wait -- A no longer holds the queue");
        CHECK(edc_task_is_ready(rb) || edc_task_await_exec(rb), "so it is runnable again");
    }

    edc_scheduler_free(sched2);
    edc_scheduler_free(sched);
    free_config(cfg);
    rmdir(ws);
}

/* ── 2c. R8: a RUNNING task is on disk before it settles ──────────────────
 *
 * The state file used to be written only when a task settled (or on a clean shutdown),
 * so `kill -9` in the middle of a long tool call left nothing behind -- after the
 * restart the task that had been running did not exist at all: not in /task list,
 * nothing for /task resume to pick up. Persistence now also happens when a task is
 * created and at each tool boundary.
 *
 * The crash is simulated the honest way: free the scheduler WITHOUT close(). close() is
 * what a clean exit runs (and it persists), so skipping it is exactly "the process was
 * killed". */
static void test_running_task_survives_a_kill(void)
{
    TEST("R8: a task is persisted when it is CREATED, not only when it settles");
    char ws[256];
    make_tmpdir(ws, sizeof(ws));
    struct edc_config *cfg = make_config(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);
    CHECK(edc_scheduler_open(sched), "open()");
    struct edc_session *sess = edc_session_manager_get_active(sched->sessions);
    CHECK(sess != NULL, "active session");
    const char *sid = sess ? edc_id_str(sess->session_id) : "";

    CHECK(state_task_rows(ws) == 0, "nothing persisted before the first task");

    struct edc_task_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.desc = "long job";
    spec.input = "do the long thing";
    spec.place = EDC_TASK_PLACE_FOREGROUND;
    spec.take_foreground = true;
    struct edc_task *t = edc_scheduler_task_create(sched, sess, &spec);
    CHECK(t != NULL, "task created");

    /* The whole point: the task is on disk NOW, while it is still running and nothing
     * has settled. */
    CHECK(state_task_rows(ws) == 1, "the task is persisted while it is still running");
    {
        sds text = read_state_text(ws);
        CHECK(text != NULL, "the state is readable");
        CHECK(text && strstr(text, "long job") != NULL, "the task description is in it");
        CHECK(text && strstr(text, "do the long thing") != NULL, "and its input (the unfinished turn)");
        sdsfree(text);
    }

    /* Make it in flight, then die the way a kill does: no close(). */
    CHECK(edc_task_await_exec(t), "task is in flight (LLM_PROCESSING)");
    const char *tid = strdup(edc_id_str(t->task_id));
    edc_scheduler_free(sched); /* NOT close(): this is the kill */

    TEST("R8: after a kill, the unfinished task is there and can be resumed");
    struct edc_scheduler *sched2 = edc_scheduler_new(cfg);
    CHECK(edc_scheduler_open(sched2), "reopen()");
    struct edc_task *back = edc_session_manager_find_task_any(sched2->sessions, (int64_t)strtoll(tid, NULL, 10), 0);
    CHECK(back != NULL, "the task that was running exists after the restart");
    /* The loader states what happened: it was ALIVE with no clean shutdown, so it comes
     * back INTERRUPTED (the same thing close() would have written). */
    CHECK(back && edc_task_is_interrupted(back), "and comes back INTERRUPTED");
    CHECK(back && edc_task_is_resumable(back), "so /task resume has something to pick up");
    CHECK(back && back->active_turn != NULL, "its unfinished turn came back too");

    edc_scheduler_free(sched2);
    free((void *)tid);
    free_config(cfg);
    rmdir(ws);
}

/* ── 3. The refusals ───────────────────────────────────────────────────── */
static void test_refusals(void)
{
    TEST("open()/close() refuse the states they cannot honour");
    CHECK(!edc_scheduler_open(NULL), "open(NULL) is refused");

    char ws[256];
    make_tmpdir(ws, sizeof(ws));
    struct edc_config *cfg = make_config(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);

    CHECK(!edc_scheduler_close(sched), "close() before open() is a no-op");

    CHECK(edc_scheduler_open(sched), "open() #1");
    /* Not re-entrant: a second open would replace a live store and leak the old
     * one, so it is refused rather than silently rebuilding. */
    CHECK(!edc_scheduler_open(sched), "open() is not re-entrant");

    edc_scheduler_free(sched);
    free_config(cfg);
    rmdir(ws);
}

/* ── 4. A borrowed store outlives the scheduler ──────────────────────────
 * The regression this guards: free() must not touch a store it did not create.
 * The fixture is freed by this test afterwards -- if free() had released it
 * already, that second free aborts the process. */
static void test_borrowed_store_is_left_alone(void)
{
    TEST("free() leaves an installed fixture store to its installer");
    char ws[256];
    make_tmpdir(ws, sizeof(ws));
    struct edc_config *cfg = make_config(ws);
    struct edc_scheduler *sched = edc_scheduler_new(cfg);

    struct edc_session *fixture = calloc(1, sizeof(struct edc_session));
    fixture->session_id = edc_test_session_id("open-close");
    fixture->config = cfg;
    struct edc_session_manager *sm = calloc(1, sizeof(struct edc_session_manager));
    sm->sessions = calloc(1, sizeof(struct edc_session *));
    sm->sessions[0] = fixture;
    sm->session_count = 1;
    sm->active_index = 0;
    sm->config = cfg;

    sched->sessions = sm; /* a fixture: sessions_owned stays false */
    CHECK(sched->sessions_owned == false, "an installed store is borrowed, not owned");
    CHECK(edc_session_manager_get_active(sched->sessions) == fixture,
          "the borrowed store is the one installed, and readable");

    edc_scheduler_free(sched);

    /* Still ours to free. A double free here means free() ignored `sessions_owned`. */
    free(sm->sessions);
    free(sm);
    free(fixture);
    CHECK(true, "free() left the borrowed store alone");

    free_config(cfg);
    rmdir(ws);
}

int main(void)
{
    edc_test_workspace(); /* disposable store for anything that writes state */
    log_info("== test_scheduler_open_close ==");
    test_new_is_in_memory_open_is_not();
    test_close_persists_then_is_idempotent();
    test_close_interrupts_in_flight_and_heals_the_queue();
    test_running_task_survives_a_kill();
    test_refusals();
    test_borrowed_store_is_left_alone();
    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
