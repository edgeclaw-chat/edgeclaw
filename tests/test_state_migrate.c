/* test_state_migrate: the schema-upgrade runner, driven through every branch.
 *
 * Why this test exists at all: the runner is the piece that decides whether a user keeps
 * their conversations across an upgrade. Its happy path is easy to eyeball; its failure
 * paths are the ones that lose data, and they are exactly the ones that never run in
 * development (nobody's step fails on the developer's machine).
 *
 * So the registry is injected. edc_migrate_run_with() lets this file supply synthetic
 * steps -- one that works, one that fails before writing, one that fails AFTER writing
 * DDL, one whose verify fails -- and assert what the runner did to the file in each case.
 * The real v7 -> v8 step is exercised too, against a hand-built v7 database, because a
 * framework with no real step is a framework nobody has proven.
 *
 * The properties under test, and the failure each one prevents:
 *
 *   1. success moves schema + data together          (half-migrated database)
 *   2. a failing step rolls back everything          (a database no version describes)
 *   3. DDL rolls back too                            (a column that "wasn't added")
 *   4. a hole in the chain writes NOTHING            (backup made, then stuck halfway)
 *   5. a newer schema is refused                     (older binary dropping new data)
 *   6. verify failures surface as failures           (silent wrong backfill)
 *   7. the backup is readable AND pre-migration      (a "backup" nobody can restore)
 *   8. row counts and row data survive               (a migration that moves data)
 */

#include "edc_state_migrate.h"
#include "edc_state_store.h"
#include "sds.h"
#include "sqlite3.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

static char g_dir[512];

/* ── Basics ─────────────────────────────────────────────────────────────────── */

static void fresh_dir(void)
{
    /* $TEST_TMPDIR is Bazel's per-test scratch (cleaned up by Bazel); /tmp is a shared
     * tmpfs that fills up on this project and produced EDQUOT instead of test failures
     * once already. Prefer the former. */
    const char *base = getenv("TEST_TMPDIR");
    if (!base || !*base)
        base = "/tmp";
    snprintf(g_dir, sizeof(g_dir), "%s/ec-migrate-XXXXXX", base);
    if (!mkdtemp(g_dir))
    {
        perror("mkdtemp");
        abort();
    }
}

static void drop_dir(void)
{
    if (!g_dir[0])
        return;
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "    (warning: could not remove %s)\n", g_dir);
    g_dir[0] = '\0';
}

static void db_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/edgeclaw.db", g_dir);
}

static bool exec_ok(sqlite3 *db, const char *sql)
{
    char *msg = NULL;
    const int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "    (sql failed: %s: %s)\n", sql, msg ? msg : "?");
        sqlite3_free(msg);
        return false;
    }
    sqlite3_free(msg);
    return true;
}

static sqlite3_int64 query_int64(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    sqlite3_int64 out = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return out;
}

static bool column_exists(sqlite3 *db, const char *table, const char *column)
{
    sds sql = sdscatprintf(sdsempty(),
                           "SELECT count(*) FROM pragma_table_info('%s') WHERE name='%s'",
                           table, column);
    const sqlite3_int64 n = query_int64(db, sql);
    sdsfree(sql);
    return n == 1;
}

/* The main-file backup written by the runner: <db>.v<from>.<stamp>, without the -wal/-shm
 * siblings that may sit beside it. */
static bool find_backup(const char *db, int from, char *out, size_t cap)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", db);
    char *slash = strrchr(dir, '/');
    const char *base = db;
    if (slash)
    {
        *slash = '\0';
        base = slash + 1;
    }

    char prefix[256];
    snprintf(prefix, sizeof prefix, "%s.v%d.", base, from);
    const size_t plen = strlen(prefix);

    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strncmp(e->d_name, prefix, plen) != 0)
            continue;
        const size_t L = strlen(e->d_name);
        if (L > 4 && (strcmp(e->d_name + L - 4, "-wal") == 0 ||
                      strcmp(e->d_name + L - 4, "-shm") == 0))
            continue;
        snprintf(out, cap, "%s/%s", dir, e->d_name);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

static int count_backup_main_files(const char *db, int from)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", db);
    char *slash = strrchr(dir, '/');
    const char *base = db;
    if (slash)
    {
        *slash = '\0';
        base = slash + 1;
    }
    char prefix[256];
    snprintf(prefix, sizeof prefix, "%s.v%d.", base, from);
    const size_t plen = strlen(prefix);

    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strncmp(e->d_name, prefix, plen) != 0)
            continue;
        const size_t L = strlen(e->d_name);
        if (L > 4 && (strcmp(e->d_name + L - 4, "-wal") == 0 ||
                      strcmp(e->d_name + L - 4, "-shm") == 0))
            continue;
        n++;
    }
    closedir(d);
    return n;
}

/* ── Fixtures ──────────────────────────────────────────────────────────────── */

/* A version-1 database with the five tables the runner counts. `seed` controls whether a
 * row is put in each one, so "the migration moved the data" is testable. */
static bool make_v1_db(const char *path, bool seed)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        return false;
    bool ok = exec_ok(db, "PRAGMA journal_mode=WAL") &&
              exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE groups(group_id INTEGER PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)");
    if (ok && seed)
    {
        ok = exec_ok(db, "INSERT INTO sessions(session_id) VALUES (1)") &&
             exec_ok(db, "INSERT INTO groups(group_id) VALUES (10)") &&
             exec_ok(db, "INSERT INTO tasks(task_id) VALUES (100)") &&
             exec_ok(db, "INSERT INTO memo_entries(entry_id) VALUES ('e1')") &&
             exec_ok(db, "INSERT INTO memo_events(seq) VALUES (1)");
    }
    ok = ok && exec_ok(db, "PRAGMA user_version=1");
    sqlite3_close(db);
    return ok;
}

/* A version-7 database, shaped like the real one for the columns this migration touches:
 * tasks.memo_seen_seq exists, memo_events has no `changed`. Two tasks, one with a
 * watermark past the origin and one at it -- the distinction the backfill is about. */
static bool make_v7_db(const char *path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        return false;
    bool ok = exec_ok(db, "PRAGMA journal_mode=WAL") &&
              exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE groups(group_id INTEGER PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY, "
                          "memo_seen_seq INTEGER NOT NULL DEFAULT 0)") &&
              exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
              /* Task 1 never saw the index; task 2 consumed events, so under v7 it was
               * necessarily sent one. */
              exec_ok(db, "INSERT INTO tasks(task_id, memo_seen_seq) VALUES (1, 0)") &&
              exec_ok(db, "INSERT INTO tasks(task_id, memo_seen_seq) VALUES (2, 5)") &&
              exec_ok(db, "PRAGMA user_version=7");
    sqlite3_close(db);
    return ok;
}

/* ── Synthetic steps ───────────────────────────────────────────────────────── */

static bool synth_add_column(sqlite3 *db, char *err, size_t cap)
{
    (void)err;
    (void)cap;
    return exec_ok(db, "ALTER TABLE sessions ADD COLUMN note TEXT");
}

static bool synth_fail_before_write(sqlite3 *db, char *err, size_t cap)
{
    (void)db;
    snprintf(err, cap, "synthetic failure before any write");
    return false;
}

/* The interesting one: DDL has already been executed when it fails. SQLite's DDL is
 * transactional, so the column must NOT be there afterwards. */
static bool synth_write_then_fail(sqlite3 *db, char *err, size_t cap)
{
    if (!exec_ok(db, "ALTER TABLE sessions ADD COLUMN half TEXT"))
        return false;
    snprintf(err, cap, "synthetic failure after a write");
    return false;
}

static bool synth_verify_fail(sqlite3 *db, char *err, size_t cap)
{
    (void)db;
    snprintf(err, cap, "synthetic verify failure");
    return false;
}

static const struct edc_migration SYNTH_OK[] = {
    {1, synth_add_column, NULL},
    {0, NULL, NULL},
};

static const struct edc_migration SYNTH_FAIL[] = {
    {1, synth_fail_before_write, NULL},
    {0, NULL, NULL},
};

static const struct edc_migration SYNTH_WRITE_THEN_FAIL[] = {
    {1, synth_write_then_fail, NULL},
    {0, NULL, NULL},
};

static const struct edc_migration SYNTH_VERIFY_FAIL[] = {
    {1, synth_add_column, synth_verify_fail},
    {0, NULL, NULL},
};

/* Target 3, but only step 2 exists: the 1 -> 2 link is the hole. */
static const struct edc_migration SYNTH_GAP[] = {
    {2, synth_add_column, NULL},
    {0, NULL, NULL},
};

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_synthetic_success(void)
{
    printf("\n── a synthetic step migrates, and the backup is a real database ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    CHECK(make_v1_db(path, true), "fixture v1 database");

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open the fixture");

    char err[1024] = {0};
    CHECK(edc_migrate_run_with(db, path, 1, SYNTH_OK, 2, err, sizeof err),
          "migration from v1 to v2 succeeds");

    CHECK(query_int64(db, "PRAGMA user_version") == 2, "user_version is now 2");
    CHECK(column_exists(db, "sessions", "note"), "the step's column exists");
    CHECK(query_int64(db, "SELECT count(*) FROM tasks") == 1, "row data survived");

    /* The backup must exist, be readable, and describe the PRE-migration schema -- a
     * copy taken after the change would restore the failure, not the data. */
    char backup[800] = {0};
    CHECK(find_backup(path, 1, backup, sizeof backup), "a backup file exists");
    if (backup[0])
    {
        sqlite3 *b = NULL;
        CHECK(sqlite3_open_v2(backup, &b, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK,
              "the backup opens");
        if (b)
        {
            char check[64] = {0};
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(b, "PRAGMA quick_check(1)", -1, &st, NULL) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW)
            {
                const char *t = (const char *)sqlite3_column_text(st, 0);
                snprintf(check, sizeof check, "%s", t ? t : "");
            }
            sqlite3_finalize(st);
            CHECK(strcmp(check, "ok") == 0, "the backup passes quick_check");
            CHECK(query_int64(b, "PRAGMA user_version") == 1,
                  "the backup is stamped with the OLD version");
            CHECK(query_int64(b, "SELECT count(*) FROM tasks") == 1,
                  "the backup holds the rows");
            sqlite3_close(b);
        }
    }
    sqlite3_close(db);
    drop_dir();
}

static void test_failure_rolls_back(void)
{
    printf("\n── a failing step leaves the database exactly as it was ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    CHECK(make_v1_db(path, true), "fixture v1 database");

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open the fixture");

    char err[1024] = {0};
    CHECK(!edc_migrate_run_with(db, path, 1, SYNTH_FAIL, 2, err, sizeof err),
          "the migration reports failure");
    CHECK(err[0] != '\0', "the failure carries a reason");
    CHECK(strstr(err, "synthetic") != NULL, "the reason is the step's own message");
    CHECK(strstr(err, "backup") != NULL, "the failure names the backup");
    /* The user must never be told to delete the database. That sentence is what used to
     * cost people their history. */
    CHECK(strstr(err, "remove") == NULL, "the failure does NOT say to remove the database");

    CHECK(query_int64(db, "PRAGMA user_version") == 1, "user_version is still 1");
    CHECK(query_int64(db, "SELECT count(*) FROM tasks") == 1, "rows are intact");
    sqlite3_close(db);
    drop_dir();
}

static void test_ddl_rolls_back(void)
{
    printf("\n── DDL written by a failing step is rolled back too ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    CHECK(make_v1_db(path, true), "fixture v1 database");

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open the fixture");

    char err[1024] = {0};
    CHECK(!edc_migrate_run_with(db, path, 1, SYNTH_WRITE_THEN_FAIL, 2, err, sizeof err),
          "the migration reports failure");
    CHECK(!column_exists(db, "sessions", "half"),
          "the column the step added is gone (transactional DDL)");
    CHECK(query_int64(db, "PRAGMA user_version") == 1, "user_version is still 1");
    sqlite3_close(db);
    drop_dir();
}

static void test_gap_writes_nothing(void)
{
    printf("\n── a hole in the chain writes nothing (not even a backup) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    CHECK(make_v1_db(path, true), "fixture v1 database");

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open the fixture");

    char err[1024] = {0};
    CHECK(!edc_migrate_run_with(db, path, 1, SYNTH_GAP, 3, err, sizeof err),
          "the migration refuses when a step is missing");
    CHECK(strstr(err, "no migration step from schema v1") != NULL,
          "the reason names the missing step");
    CHECK(query_int64(db, "PRAGMA user_version") == 1, "user_version is unchanged");
    CHECK(count_backup_main_files(path, 1) == 0,
          "no backup was written -- the chain is checked before any work");
    sqlite3_close(db);
    drop_dir();
}

static void test_newer_version_refused(void)
{
    printf("\n── a database from a NEWER build is refused, and said so ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    CHECK(make_v1_db(path, true), "fixture v1 database");

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open the fixture");

    char err[1024] = {0};
    CHECK(!edc_migrate_run_with(db, path, 5, SYNTH_OK, 2, err, sizeof err),
          "a v5 database is refused by a build that understands v2");
    CHECK(strstr(err, "newer EdgeClaw") != NULL, "the reason says a newer build wrote it");
    CHECK(strstr(err, "remove") == NULL, "the reason does not tell anyone to delete data");
    CHECK(query_int64(db, "PRAGMA user_version") == 1, "the file is untouched");
    sqlite3_close(db);
    drop_dir();
}

static void test_no_version_refused(void)
{
    printf("\n── a database with no version stamp is refused ──\n");
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK, "in-memory connection");
    char err[512] = {0};
    CHECK(!edc_migrate_run_with(db, ":memory:", 0, SYNTH_OK, 2, err, sizeof err),
          "from=0 is refused");
    CHECK(strstr(err, "no version stamp") != NULL, "the reason explains why");
    sqlite3_close(db);
}

static void test_verify_failure_reported(void)
{
    printf("\n── a step whose verify fails is a failed migration ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    CHECK(make_v1_db(path, true), "fixture v1 database");

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open the fixture");

    char err[1024] = {0};
    CHECK(!edc_migrate_run_with(db, path, 1, SYNTH_VERIFY_FAIL, 2, err, sizeof err),
          "the verify failure is a failure");
    CHECK(strstr(err, "synthetic verify failure") != NULL,
          "the verify message is what the user sees");
    /* The step itself committed; only the post-condition failed. Saying so is better than
     * pretending the file is untouched. */
    CHECK(query_int64(db, "PRAGMA user_version") == 2,
          "the step stayed committed (the file is at a complete version)");
    sqlite3_close(db);
    drop_dir();
}

/* The real thing: v7 -> v8, against a database shaped like the one a v1.6.2 user has. */
static void test_real_v7_to_v8(void)
{
    printf("\n── the real v7 -> v8 step (what a v1.6.2 user hits) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    CHECK(make_v7_db(path), "fixture v7 database");

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open the fixture");

    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 7, err, sizeof err), "v7 migrates to v8");

    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is v8");
    CHECK(column_exists(db, "tasks", "memo_baseline_done"),
          "tasks.memo_baseline_done exists");
    CHECK(column_exists(db, "memo_events", "changed"), "memo_events.changed exists");

    /* The backfill: the task that had consumed events is baselined; the one sitting at
     * the origin is not. This is the whole semantic point of the column. */
    CHECK(query_int64(db, "SELECT memo_baseline_done FROM tasks WHERE task_id=1") == 0,
          "a task with memo_seen_seq == 0 is NOT baselined");
    CHECK(query_int64(db, "SELECT memo_baseline_done FROM tasks WHERE task_id=2") == 1,
          "a task with memo_seen_seq > 0 IS baselined");
    CHECK(query_int64(db, "SELECT count(*) FROM memo_events") == 0,
          "memo_events rows survive (none in this fixture)");

    /* And the backup is the v7 file, so a user can go back. */
    char backup[800] = {0};
    CHECK(find_backup(path, 7, backup, sizeof backup), "a v7 backup exists");
    if (backup[0])
    {
        sqlite3 *b = NULL;
        if (sqlite3_open_v2(backup, &b, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
        {
            CHECK(query_int64(b, "PRAGMA user_version") == 7,
                  "the backup is still v7");
            sqlite3_close(b);
        }
    }
    sqlite3_close(db);
    drop_dir();
}

/* Running it twice: the second run must refuse (there is nothing to do), not "succeed"
 * silently and not corrupt anything. That is what makes a retry after a crash safe. */
static void test_rerun_is_refused(void)
{
    printf("\n── re-running against an already-migrated file is refused ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    CHECK(make_v7_db(path), "fixture v7 database");

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open the fixture");

    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 7, err, sizeof err), "first run migrates");
    err[0] = '\0';
    CHECK(!edc_migrate_run(db, path, 8, err, sizeof err), "second run is refused");
    CHECK(strstr(err, "newer EdgeClaw") != NULL,
          "a retry at the current version reports 'nothing to do' as a refusal");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "still v8");
    CHECK(query_int64(db, "SELECT count(*) FROM tasks") == 2, "data is intact");
    sqlite3_close(db);
    drop_dir();
}

int main(void)
{
    printf("test_state_migrate\n");
    test_synthetic_success();
    test_failure_rolls_back();
    test_ddl_rolls_back();
    test_gap_writes_nothing();
    test_newer_version_refused();
    test_no_version_refused();
    test_verify_failure_reported();
    test_real_v7_to_v8();
    test_rerun_is_refused();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
