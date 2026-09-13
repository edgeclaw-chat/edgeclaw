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
#include "edc_config.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "log.h"
#include "sds.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    system(cmd);
}

/* Numeric suffix of an "s<N>" / "sess-<N>" id; -1 when it is not that shape. */
static long long sid_num(const char *id)
{
    if (!id)
        return -1;
    if (strncmp(id, "sess-", 5) == 0)
        id += 5;
    else if (id[0] == 's')
        id += 1;
    else
        return -1;

    char *end = NULL;
    long long n = strtoll(id, &end, 10);
    if (end == id || (end && *end != '\0'))
        return -1;
    return n;
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

    CHECK(a && b && strcmp(a->session_id, b->session_id) != 0, "A and B have different ids");
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
        snprintf(removed_id, sizeof(removed_id), "%s", b->session_id);
        snprintf(removed_dir, sizeof(removed_dir), "%s", b->history_dir);
    }

    CHECK(edc_session_manager_stop(sm, removed_id), "B removed");

    struct edc_session *c = edc_session_manager_create_ex(sm, "C", NULL);
    CHECK(c != NULL, "session C created");
    CHECK(c && strcmp(c->session_id, removed_id) != 0, "C does not reuse the removed id");
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
        snprintf(removed_id, sizeof(removed_id), "%s", b->session_id);
    CHECK(edc_session_manager_stop(sm, removed_id), "B removed");
    edc_session_manager_free(sm);

    /* Restart: a fresh manager on the same workspace. With ids derived from the
     * surviving sessions' max+1, B's id would be recomputed and handed out. */
    struct edc_session_manager *sm2 = edc_session_manager_new(cfg);
    CHECK(sm2 != NULL, "restarted manager created");
    struct edc_session *c = edc_session_manager_create_ex(sm2, "C", NULL);
    CHECK(c != NULL, "session C created after restart");
    CHECK(c && strcmp(c->session_id, removed_id) != 0, "C does not reuse the removed id after restart");
    CHECK(c && sid_num(c->session_id) > sid_num(removed_id), "C's id is beyond the removed one");

    edc_session_manager_free(sm2);
    edc_config_free(cfg);
    wipe_workspace(ws);
}

/* ── 4. An orphan directory seeds the high-water (upgrade path) ──────────── */
static void test_orphan_dir_seeding(void)
{
    TEST("ids skip session directories left by removed sessions");
    char ws[256];
    make_workspace(ws, sizeof(ws));

    struct edc_config *cfg = make_config(ws);
    struct edc_session_manager *sm = edc_session_manager_new(cfg);
    CHECK(sm != NULL, "manager created");
    edc_session_manager_free(sm);

    /* Simulate an upgraded workspace: no high-water file (pre-change archives),
     * and a removed session whose directory is still on disk but no longer in
     * the index. */
    char seq_path[512];
    snprintf(seq_path, sizeof(seq_path), "%s/.session_seq", ws);
    unlink(seq_path);
    char orphan[512];
    snprintf(orphan, sizeof(orphan), "%s/s7", ws);
    CHECK(mkdir(orphan, 0755) == 0, "orphan session directory created");

    struct edc_session_manager *sm2 = edc_session_manager_new(cfg);
    CHECK(sm2 != NULL, "manager created on the upgraded workspace");
    struct edc_session *c = edc_session_manager_create_ex(sm2, "C", NULL);
    CHECK(c != NULL, "session C created");
    CHECK(c && sid_num(c->session_id) > 7, "C's id is past the orphan directory's number");
    CHECK(c && strcmp(c->session_id, "s7") != 0, "C does not collide with the orphan directory");
    char new_dir[512];
    if (c)
    {
        snprintf(new_dir, sizeof(new_dir), "%s", c->history_dir);
        CHECK(!dir_exists(new_dir), "C's directory was not already occupied");
    }

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
    test_orphan_dir_seeding();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
