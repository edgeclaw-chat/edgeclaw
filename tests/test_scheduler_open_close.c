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
#include "edc_file_utils.h"
#include "edc_scheduler.h"
#include "edc_session_manager.h"
#include "edc_session.h"
#include "log.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

volatile sig_atomic_t _run_loop = 1;

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
    CHECK(active != NULL && active->session_id && *active->session_id,
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
    const char *sid = active ? active->session_id : NULL;
    CHECK(sid != NULL && *sid, "active session id");

    char path[512];
    snprintf(path, sizeof(path), "%s/%s/session.json", ws, sid ? sid : "");
    CHECK(access(path, F_OK) != 0, "no state file before close()");

    CHECK(edc_scheduler_close(sched), "first close() settles");
    CHECK(access(path, F_OK) == 0, "close() wrote the session state file");

    /* Idempotent: the second call reports it did nothing. Re-running the save
     * would be harmless but pointless, and the flag is what makes that visible. */
    CHECK(!edc_scheduler_close(sched), "second close() is a no-op");

    edc_scheduler_free(sched);
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
    log_info("== test_scheduler_open_close ==");
    test_new_is_in_memory_open_is_not();
    test_close_persists_then_is_idempotent();
    test_refusals();
    test_borrowed_store_is_left_alone();
    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
