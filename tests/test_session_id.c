/* Session-id uniqueness.
 *
 * A session id must never be reused. The id keys the session's whole on-disk
 * footprint — <workspace>/<id>/{session.json,chat.jsonl,tasks.idx} — so if
 * /session remove is followed by /session new handing out the same id, the new
 * session lands on the removed one's directory: it replays the old chat
 * transcript and cross-session task lookups resolve to the wrong owner.
 *
 * Ids are issued from a monotonic high-water persisted in <workspace>/.session_seq
 * and seeded (for pre-existing workspaces) from the ids in sessions.json plus
 * the session directories already on disk, including ones left behind by
 * removed sessions.
 */

#include "edc_compat.h"
#include "edc_task.h"  /* edc_id_str() */
#include "edc_config.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_state_store.h" /* edc_state_store_has_session: "the dropped row is really gone" */
#include "log.h"
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_fs_util.h"

/* Stub: referenced transitively by the shell/command layer. */
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

static struct edc_config *make_config(const char *workspace)
{
    struct edc_config *cfg = g_new0(struct edc_config, 1);
    cfg->workspace = sdsnew(workspace);
    return cfg;
}

static void make_workspace(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/edc_sid_test_XXXXXX");
    if (!mkdtemp(buf))
    {
        perror("mkdtemp");
        exit(2);
    }
}

static void wipe_workspace(const char *ws)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    test_sh(cmd);
}

/* Numeric suffix of an "s<N>" / "sess-<N>" id; -1 when it is not that shape. */
/* The id is a number now (INTEGER PRIMARY KEY AUTOINCREMENT), so "the number in it" is
 * the value itself. Kept as a function so the assertions below still read as
 * "B's number is beyond A's". */
static long long sid_num(int64_t id)
{
    return (long long)id;
}

static bool dir_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ── 1. Two sessions get distinct ids and directories ────────────────────── */
static void test_ids_advance(void)
{
    TEST("sessions created in one run get distinct, increasing ids");
    char ws[256];
    make_workspace(ws, sizeof(ws));

    struct edc_config *cfg = make_config(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    CHECK(sm != NULL, "manager created");

    struct edc_session *a = edc_session_manager_get_active(sm);
    CHECK(a != NULL, "default session exists");
    struct edc_session *b = edc_session_manager_create_ex(sm, "B", NULL);
    CHECK(b != NULL, "session B created");

    CHECK(a && b && a->session_id != b->session_id, "A and B have different ids");
    CHECK(a && b && strcmp(a->history_dir, b->history_dir) != 0, "A and B use different directories");
    CHECK(a && b && sid_num(b->session_id) > sid_num(a->session_id), "B's id is beyond A's");

    edc_session_manager_free(sm);
    edc_config_free(cfg);
    wipe_workspace(ws);
}

/* ── 2. Removing a session does not free its id for reuse (same run) ─────── */
static void test_no_reuse_after_remove(void)
{
    TEST("a removed session's id is not handed out again");
    char ws[256];
    make_workspace(ws, sizeof(ws));

    struct edc_config *cfg = make_config(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    CHECK(sm != NULL, "manager created");

    struct edc_session *b = edc_session_manager_create_ex(sm, "B", NULL);
    CHECK(b != NULL, "session B created");
    char removed_id[64] = {0};
    char removed_dir[512] = {0};
    if (b)
    {
        snprintf(removed_id, sizeof(removed_id), "%s", edc_id_str(b->session_id));
        snprintf(removed_dir, sizeof(removed_dir), "%s", b->history_dir);
    }

    CHECK(edc_session_manager_stop(sm, removed_id), "B removed");

    struct edc_session *c = edc_session_manager_create_ex(sm, "C", NULL);
    CHECK(c != NULL, "session C created");
    CHECK(c && strcmp(edc_id_str(c->session_id), removed_id) != 0,
          "C does not reuse the removed id");
    CHECK(c && strcmp(c->history_dir, removed_dir) != 0, "C does not land on the removed directory");

    edc_session_manager_free(sm);
    edc_config_free(cfg);
    wipe_workspace(ws);
}

/* ── 3. The removed max id survives a restart (not recomputed) ───────────── */
static void test_no_reuse_across_restart(void)
{
    TEST("a restart does not go back and reuse a removed id");
    char ws[256];
    make_workspace(ws, sizeof(ws));

    struct edc_config *cfg = make_config(ws);

    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    CHECK(sm != NULL, "first manager created");
    struct edc_session *b = edc_session_manager_create_ex(sm, "B", NULL);
    CHECK(b != NULL, "session B created");
    char removed_id[64] = {0};
    if (b)
        snprintf(removed_id, sizeof(removed_id), "%s", edc_id_str(b->session_id));
    CHECK(edc_session_manager_stop(sm, removed_id), "B removed");
    edc_session_manager_free(sm);

    /* Restart: a fresh manager on the same workspace. With ids derived from the
     * surviving sessions' max+1, B's id would be recomputed and handed out. */
    struct edc_session_manager *sm2 = edc_session_manager_new(cfg);
    CHECK(sm2 != NULL, "restarted manager created");
    struct edc_session *c = edc_session_manager_create_ex(sm2, "C", NULL);
    CHECK(c != NULL, "session C created after restart");
    CHECK(c && strcmp(edc_id_str(c->session_id), removed_id) != 0,
          "C does not reuse the removed id after restart");
    CHECK(c && (long long)c->session_id > strtoll(removed_id + (removed_id[0] == 's' ? 1 : 0), NULL, 10),
          "C's id is beyond the removed one");

    edc_session_manager_free(sm2);
    edc_config_free(cfg);
    wipe_workspace(ws);
}

/* ── 4b. The allocation loop: an id whose directory is taken is given back ─────
 *
 * edc_session_manager_create_ex allocates a row, checks that <ws>/<id> is free, and — when
 * it is not — calls edc_state_store_delete_session() to give the id back before asking for
 * the next one. That call is the only reason a removed session's leftover directory cannot
 * hand its transcript to a new session, and until now nothing exercised it.
 *
 * The trigger has to be built deliberately. AUTOINCREMENT hands out max+1 and never
 * reuses, so "an orphan directory exists somewhere" is not enough — the directory has to be
 * for the id the NEXT insert will receive. The version of this case that pre-created "s7"
 * could not collide (the first session already owns id 1, so the next insert asks for 2)
 * and asserted `id != 7`, which was true for the wrong reason.
 *
 * Note the directory is NOT removed afterwards — that is the point of the guard: /session
 * remove deletes the row and leaves <ws>/<id> behind to hold chat.jsonl. */
static void test_taken_directory_gives_the_id_back(void)
{
    TEST("an id whose directory is already taken is dropped, and the next one is used");
    char ws[256];
    make_workspace(ws, sizeof(ws));

    struct edc_config *cfg = make_config(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    CHECK(sm != NULL, "manager created");

    /* The default session owns the first id; the next insert will ask for the one after. */
    struct edc_session *first = edc_session_manager_get_active(sm);
    CHECK(first != NULL, "the default session exists");
    const int64_t taken_id = first ? first->session_id + 1 : 2;

    char taken_dir[600];
    snprintf(taken_dir, sizeof(taken_dir), "%s/%lld", ws, (long long)taken_id);
    CHECK(dir_exists(taken_dir) == false, "the directory we are about to squat on is free");
    CHECK(mkdir(taken_dir, 0755) == 0, "squatted on the directory of the next id");

    struct edc_session *c = edc_session_manager_create_ex(sm, "C", NULL);
    CHECK(c != NULL, "the session was still created");
    CHECK(c && c->session_id == taken_id + 1,
          "it landed on the id AFTER the taken one (the loop asked for the next)");
    /* The directory itself is created on first save, not here, so what can be asserted is
     * that history_dir names the id that survived. */
    char expect_dir[600];
    snprintf(expect_dir, sizeof(expect_dir), "%s/%lld", ws, (long long)(taken_id + 1));
    CHECK(c && strcmp(c->history_dir, expect_dir) == 0, "and its history_dir names that id");

    /* The strong half: the squatted id's ROW is gone, not merely skipped. If the loop just
     * ignored it, an orphan row with no owner would be left in the database. */
    CHECK(!edc_state_store_has_session(taken_id), "the dropped id has no row left behind");
    CHECK(edc_state_store_has_session(c ? c->session_id : 0), "the surviving id does have one");
    CHECK(dir_exists(taken_dir), "the squatted directory is untouched (it holds the old transcript)");

    edc_session_manager_free(sm);
    edc_config_free(cfg);
    wipe_workspace(ws);
}

/* ── 4. A leftover directory elsewhere does not block creation ───────────── */
static void test_orphan_dir_seeding(void)
{
    TEST("a stale session directory elsewhere does not disturb a new session");
    char ws[256];
    make_workspace(ws, sizeof(ws));

    struct edc_config *cfg = make_config(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    CHECK(sm != NULL, "manager created");
    edc_session_manager_free(sm);

    /* Simulate a workspace that has been used before: a removed session whose directory is
     * still on disk but no longer in the index. The id is deliberately NOT the one the next
     * insert will receive — the collision case is the point of
     * test_taken_directory_gives_the_id_back, and this case is the benign one: the leftover
     * is somewhere else and must cost nothing. */
    char orphan[512];
    snprintf(orphan, sizeof(orphan), "%s/s7", ws);
    CHECK(mkdir(orphan, 0755) == 0, "a stale session directory exists");

    struct edc_session_manager *sm2 = edc_session_manager_new(cfg);
    CHECK(sm2 != NULL, "manager created on the used workspace");
    struct edc_session *c = edc_session_manager_create_ex(sm2, "C", NULL);
    CHECK(c != NULL, "session C created");
    CHECK(c && c->session_id != 7, "C did not land on the stale directory's name");
    CHECK(c && !dir_exists(c->history_dir),
          "C's directory was not already occupied (creation does not reuse a name)");
    CHECK(dir_exists(orphan), "the stale directory was left alone");

    edc_session_manager_free(sm2);
    edc_config_free(cfg);
    wipe_workspace(ws);
}

int main(void)
{
    log_info("== test_session_id ==");

    test_ids_advance();
    test_no_reuse_after_remove();
    test_no_reuse_across_restart();
    test_taken_directory_gives_the_id_back();
    test_orphan_dir_seeding();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
