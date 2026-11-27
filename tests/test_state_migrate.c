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

#include "edc_chat_bubble_line.h"
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
#include "test_fs_util.h"

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
    if (test_sh(cmd) != 0)
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

/* First TEXT column of the first row, or "" -- for asserting migrated field values. */
static void query_text(sqlite3 *db, const char *sql, char *out, size_t cap)
{
    sqlite3_stmt *st = NULL;
    out[0] = '\0';
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        snprintf(out, cap, "%s", t ? t : "");
    }
    sqlite3_finalize(st);
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
              exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, workdir TEXT)") &&
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
              exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, workdir TEXT)") &&
              exec_ok(db, "CREATE TABLE groups(group_id INTEGER PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY, "
                          "memo_seen_seq INTEGER NOT NULL DEFAULT 0)") &&
              exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
              exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
              /* Task 1 never saw the index; task 2 consumed events, so under v7 it was
               * necessarily sent one. */
              exec_ok(db, "INSERT INTO tasks(task_id, memo_seen_seq) VALUES (1, 0)") &&
              exec_ok(db, "INSERT INTO tasks(task_id, memo_seen_seq) VALUES (2, 5)") &&
              /* One group, so the v10 backfill assertion below has a real row to read;
               * an empty table would make "no non-default rows" pass vacuously. */
              exec_ok(db, "INSERT INTO groups(group_id) VALUES (10)") &&
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
    CHECK(edc_migrate_run(db, path, 7, err, sizeof err), "v7 migrates to the current version");

    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
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
    /* v9 is additive: the v8 step still ran, and the new table exists too. */
    CHECK(query_int64(db, "SELECT count(*) FROM sqlite_master "
                           "WHERE type='table' AND name='chat_bubbles'") == 1,
          "chat_bubbles exists after migrating through v9");
    /* v10 is additive too: the column exists on a migrated database, and its default
     * keeps every pre-v10 group at the full cap (0). A non-zero default here would
     * silently shrink the corpus of every group in a user's existing database. */
    CHECK(column_exists(db, "groups", "fold_corpus_budget"),
          "groups.fold_corpus_budget exists after migrating through v10");
    CHECK(query_int64(db, "SELECT count(*) FROM groups WHERE group_id=10") == 1,
          "the v10 backfill assertion has a real pre-existing group to read");
    CHECK(query_int64(db, "SELECT fold_corpus_budget FROM groups WHERE group_id=10") == 0,
          "v10 backfills the full-cap sentinel 0 into an existing group");

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

/* The real v12 -> v13 step: read_count is RENAMED to get_count (keeping recorded
 * values) and search_count is added at 0. A v12 DB with a real read_count value
 * is the fixture -- an empty table would make "kept the value" vacuous. */
static void test_real_v12_to_v13(void)
{
    printf("\n── the real v12 -> v13 step (usage counters split) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, workdir TEXT)") &&
                 exec_ok(db, "CREATE TABLE groups(group_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY, "
                             "read_count INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "INSERT INTO memo_entries(entry_id,read_count) VALUES ('m1',7)") &&
                 exec_ok(db, "PRAGMA user_version=12");
    CHECK(built, "fixture v12 database with a real read_count=7");
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 12, err, sizeof err), "v12 migrates to v13");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
    CHECK(column_exists(db, "memo_entries", "get_count"), "get_count exists");
    CHECK(column_exists(db, "memo_entries", "search_count"), "search_count exists");
    CHECK(!column_exists(db, "memo_entries", "read_count"), "old read_count column is gone");
    CHECK(query_int64(db, "SELECT get_count FROM memo_entries WHERE entry_id='m1'") == 7,
          "recorded read_count=7 became get_count=7 (no data loss)");
    CHECK(query_int64(db, "SELECT search_count FROM memo_entries WHERE entry_id='m1'") == 0,
          "search_count starts at 0 (unrecorded history not invented)");
    sqlite3_close(db);
    drop_dir();
}

/* The real v13 -> v14 step: an independent extract budget is added at 0. */
static void test_real_v13_to_v14(void)
{
    printf("\n── the real v13 -> v14 step (independent extract budget) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, workdir TEXT)") &&
                 exec_ok(db, "CREATE TABLE groups(group_id INTEGER PRIMARY KEY, "
                             "fold_corpus_budget INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "INSERT INTO groups(group_id,fold_corpus_budget) VALUES (10,32768)") &&
                 exec_ok(db, "PRAGMA user_version=13");
    CHECK(built, "fixture v13 database with a real group");
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 13, err, sizeof err), "v13 migrates to v14");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
    CHECK(column_exists(db, "groups", "extract_summary_budget"), "extract_summary_budget exists");
    CHECK(query_int64(db, "SELECT extract_summary_budget FROM groups WHERE group_id=10") == 0,
          "new extract budget starts at full-cap sentinel 0");
    CHECK(query_int64(db, "SELECT fold_corpus_budget FROM groups WHERE group_id=10") == 32768,
          "the independent fold budget value survives migration");
    sqlite3_close(db);
    drop_dir();
}

/* The real v14 -> v15 step: the jobs table (and its index) is created; no data is
 * imported from the old jobs.json; existing rows are untouched. */
static void test_real_v14_to_v15(void)
{
    printf("\n── the real v14 -> v15 step (jobs table) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, workdir TEXT)") &&
                 exec_ok(db, "CREATE TABLE groups(group_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE chat_bubbles(session_id TEXT, seq INTEGER, line TEXT)") &&
                 exec_ok(db, "INSERT INTO tasks(task_id) VALUES (42)") &&
                 exec_ok(db, "PRAGMA user_version=14");
    CHECK(built, "fixture v14 database with a real task and no jobs table");
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 14, err, sizeof err), "v14 migrates to v15");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
    CHECK(column_exists(db, "jobs", "job_id"), "jobs.job_id exists");
    CHECK(column_exists(db, "jobs", "next_run_at"), "jobs.next_run_at exists");
    CHECK(query_int64(db, "SELECT count(*) FROM jobs") == 0, "jobs starts empty (no jobs.json import)");
    CHECK(query_int64(db,
                      "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='jobs_by_next_run'") == 1,
          "jobs_by_next_run index exists");
    CHECK(query_int64(db, "SELECT count(*) FROM tasks") == 1, "existing task survives migration");
    sqlite3_close(db);
    drop_dir();
}

/* The real v15 -> v16 step: sessions.workdir is made NOT NULL.
 *
 * The fixture has a NULL workdir (the legacy "unbound" state), a real path, AND a child
 * group with an ON DELETE CASCADE foreign key. The last one guards the implementation
 * choice: an ALTER COLUMN SET NOT NULL keeps the parent table in place, so the child row
 * must survive -- a table rebuild would rename sessions out from under the child's FK
 * and the cascade would silently wipe it. */
static void test_real_v15_to_v16(void)
{
    printf("\n── the real v15 -> v16 step (workdir NOT NULL) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "PRAGMA foreign_keys=ON") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, "
                             "name TEXT NOT NULL, workdir TEXT, "
                             "is_active INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE UNIQUE INDEX sessions_one_active ON sessions(is_active) "
                             "WHERE is_active=1") &&
                 exec_ok(db, "CREATE TABLE groups(session_id INTEGER NOT NULL REFERENCES "
                             "sessions(session_id) ON DELETE CASCADE, group_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "INSERT INTO sessions(session_id,name,workdir) VALUES(1,'legacy',NULL)") &&
                 exec_ok(db, "INSERT INTO sessions(session_id,name,workdir) VALUES(2,'bound','/a/b')") &&
                 exec_ok(db, "INSERT INTO groups(session_id,group_id) VALUES(1,10)") &&
                 exec_ok(db, "PRAGMA user_version=15");
    CHECK(built, "fixture v15 database with a NULL workdir, a real one and a child row");
    /* Anti-vacuum: prove the precondition this test is about really exists. */
    CHECK(query_int64(db, "SELECT count(*) FROM sessions WHERE workdir IS NULL") == 1,
          "fixture really has one NULL workdir");
    CHECK(query_int64(db, "SELECT \"notnull\" FROM pragma_table_info('sessions') WHERE name='workdir'") == 0,
          "fixture workdir is nullable before migration");
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 15, err, sizeof err), "v15 migrates to v16");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
    CHECK(query_int64(db, "SELECT \"notnull\" FROM pragma_table_info('sessions') WHERE name='workdir'") == 1,
          "sessions.workdir is now NOT NULL");
    CHECK(query_int64(db, "SELECT count(*) FROM sessions WHERE workdir IS NULL") == 0,
          "no NULL workdir remains");
    CHECK(query_int64(db, "SELECT count(*) FROM sessions WHERE workdir=''") == 1,
          "the legacy NULL row was backfilled to '' (unbound)");
    CHECK(query_int64(db, "SELECT count(*) FROM sessions WHERE workdir='/a/b'") == 1,
          "an existing real path is unchanged");
    CHECK(query_int64(db, "SELECT count(*) FROM groups WHERE group_id=10") == 1,
          "the child group survives (no cascade from a table rebuild)");
    /* Migrated databases have no column DEFAULT (this SQLite build cannot ALTER one in),
     * so the writing paths pass workdir explicitly. Prove that shape still inserts. */
    CHECK(exec_ok(db, "INSERT INTO sessions(session_id,name,workdir,updated_at) "
                      "VALUES(3,'fresh','',strftime('%s','now'))"),
          "a new session can be inserted with explicit '' workdir");
    CHECK(query_int64(db, "SELECT count(*) FROM sessions WHERE workdir=''") == 2,
          "the freshly inserted unbound session is stored as ''");
    CHECK(query_int64(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0,
          "no foreign key violations after migration");
    sqlite3_close(db);
    drop_dir();
}

/* The real v16 -> v17 step: group retirement columns. Both are additive DEFAULT 0, so
 * every pre-v17 group reads back active; existing groups and their data are untouched. */
static void test_real_v16_to_v17(void)
{
    printf("\n── the real v16 -> v17 step (group archive columns) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "PRAGMA foreign_keys=ON") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, "
                             "name TEXT NOT NULL, workdir TEXT NOT NULL DEFAULT '', updated_at INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE groups(session_id INTEGER NOT NULL REFERENCES "
                             "sessions(session_id) ON DELETE CASCADE, group_id INTEGER PRIMARY KEY, "
                             "is_orphaned INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "INSERT INTO sessions(session_id,name) VALUES(1,'s')") &&
                 exec_ok(db, "INSERT INTO groups(session_id,group_id) VALUES(1,10)") &&
                 exec_ok(db, "INSERT INTO groups(session_id,group_id) VALUES(1,11)") &&
                 exec_ok(db, "PRAGMA user_version=16");
    CHECK(built, "fixture v16 database with two groups");
    CHECK(!column_exists(db, "groups", "is_archived") && !column_exists(db, "groups", "idle_since"),
          "fixture has no retirement/idle columns before migration");
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 16, err, sizeof err), "v16 migrates to v17");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
    CHECK(column_exists(db, "groups", "is_archived") &&
              column_exists(db, "groups", "archived_at") &&
              column_exists(db, "groups", "idle_since"),
          "all three v17 columns exist");
    CHECK(query_int64(db, "SELECT count(*) FROM groups WHERE is_archived!=0 OR archived_at!=0 OR idle_since!=0") == 0,
          "all pre-existing groups default to active / unknown idle");
    CHECK(query_int64(db, "SELECT count(*) FROM groups") == 2, "group rows survive");
    /* Defaulted columns accept an explicit write (the writer path binds all three). */
    CHECK(exec_ok(db, "UPDATE groups SET is_archived=1, archived_at=12345, idle_since=999 WHERE group_id=10"),
          "retirement/idle fields can be written");
    CHECK(query_int64(db, "SELECT archived_at FROM groups WHERE group_id=10") == 12345,
          "archived_at round-trips");
    CHECK(query_int64(db, "SELECT idle_since FROM groups WHERE group_id=10") == 999,
          "idle_since round-trips");
    CHECK(query_int64(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0,
          "no foreign key violations after migration");
    sqlite3_close(db);
    drop_dir();
}

/* The real v17 -> v18 repair: an incomplete v17 stamped database (an early build added
 * only is_archived / archived_at, not idle_since) must gain the missing column. This is the
 * exact shape of the database that made every group vanish from the sidebar: the load SQL
 * referenced idle_since, failed to prepare, and the session was quarantined. */
static void test_real_v17_repair(void)
{
    printf("\n── the real v17 -> v18 repair (incomplete v17 database) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "PRAGMA foreign_keys=ON") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, "
                             "name TEXT NOT NULL, workdir TEXT NOT NULL DEFAULT '', updated_at INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE groups(session_id INTEGER NOT NULL REFERENCES "
                             "sessions(session_id) ON DELETE CASCADE, group_id INTEGER PRIMARY KEY, "
                             "is_orphaned INTEGER NOT NULL DEFAULT 0, "
                             "is_archived INTEGER NOT NULL DEFAULT 0, "
                             "archived_at INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "INSERT INTO sessions(session_id,name) VALUES(1,'s')") &&
                 exec_ok(db, "INSERT INTO groups(session_id,group_id,is_archived) VALUES(1,10,1)") &&
                 exec_ok(db, "INSERT INTO groups(session_id,group_id) VALUES(1,11)") &&
                 exec_ok(db, "PRAGMA user_version=17");
    CHECK(built, "fixture: a v17 database missing idle_since, with an archived group");
    CHECK(column_exists(db, "groups", "is_archived") &&
              column_exists(db, "groups", "archived_at") &&
              !column_exists(db, "groups", "idle_since"),
          "fixture has is_archived / archived_at but no idle_since");
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 17, err, sizeof err), "broken v17 migrates to v18");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
    CHECK(column_exists(db, "groups", "idle_since"), "idle_since was added by the repair");
    CHECK(query_int64(db, "SELECT count(*) FROM groups") == 2, "group rows survive the repair");
    CHECK(query_int64(db, "SELECT idle_since FROM groups WHERE group_id=10") == 0,
          "repaired idle_since defaults to 0");
    CHECK(query_int64(db, "SELECT is_archived FROM groups WHERE group_id=10") == 1,
          "existing archived flag is preserved, not reset");
    CHECK(query_int64(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0,
          "no foreign key violations after repair");
    sqlite3_close(db);
    drop_dir();
}

/* A wholly-unretired v17 (ALL three retirement columns missing) is the other broken
 * shape the repair must handle. It exercises each idempotent ADD COLUMN: a build that
 * stamped v17 before adding ANY of the columns must gain all three, all defaulted to 0. */
static void test_real_v17_repair_all_columns_missing(void)
{
    printf("\n── the v17 -> v18 repair (v17 with no retirement/idle columns at all) ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "PRAGMA foreign_keys=ON") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, "
                             "name TEXT NOT NULL, workdir TEXT NOT NULL DEFAULT '', updated_at INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE groups(session_id INTEGER NOT NULL REFERENCES "
                             "sessions(session_id) ON DELETE CASCADE, group_id INTEGER PRIMARY KEY, "
                             "is_orphaned INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "INSERT INTO sessions(session_id,name) VALUES(1,'s')") &&
                 exec_ok(db, "INSERT INTO groups(session_id,group_id) VALUES(1,10)") &&
                 exec_ok(db, "INSERT INTO groups(session_id,group_id) VALUES(1,11)") &&
                 exec_ok(db, "PRAGMA user_version=17");
    CHECK(built, "fixture: a v17 database with none of the retirement/idle columns");
    CHECK(!column_exists(db, "groups", "is_archived") &&
              !column_exists(db, "groups", "archived_at") &&
              !column_exists(db, "groups", "idle_since"),
          "fixture has none of the three columns before repair");
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 17, err, sizeof err), "v17 with no columns migrates to v18");
    CHECK(column_exists(db, "groups", "is_archived") &&
              column_exists(db, "groups", "archived_at") &&
              column_exists(db, "groups", "idle_since"),
          "all three columns were added by the repair");
    CHECK(query_int64(db, "SELECT count(*) FROM groups") == 2, "both group rows survive");
    CHECK(query_int64(db, "SELECT count(*) FROM groups WHERE is_archived!=0 OR archived_at!=0 OR idle_since!=0") == 0,
          "all repaired groups default to active / unknown idle");
    sqlite3_close(db);
    drop_dir();
}

/* v18 -> v19 splits chat_bubbles' single encoded line into typed columns. This
 * fixture carries REAL legacy rows and asserts their fields survive the rewrite. */
static void test_v18_to_v19_with_chat_bubbles(void)
{
    printf("\n── v18 -> v19 splits real chat_bubbles rows ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");

    /* Minimal v18 schema: the tables the post-migration verify expects, plus the
     * OLD single-line chat_bubbles. */
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, "
                             "name TEXT NOT NULL, workdir TEXT NOT NULL DEFAULT '', updated_at INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE groups(session_id INTEGER NOT NULL, group_id INTEGER PRIMARY KEY, "
                             "is_orphaned INTEGER NOT NULL DEFAULT 0, is_archived INTEGER NOT NULL DEFAULT 0, "
                             "archived_at INTEGER NOT NULL DEFAULT 0, idle_since INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE chat_bubbles(session_id TEXT NOT NULL, seq INTEGER NOT NULL, "
                             "line TEXT NOT NULL, PRIMARY KEY(session_id,seq))") &&
                 exec_ok(db, "PRAGMA user_version=18");
    CHECK(built, "fixture: a v18 database with the old single-line chat_bubbles");

    /* Two legacy encoded lines: a tool row with escaped content and a plain reply. */
    char *line1 = edc_chat_bubble_encode("tool", 0, "task-7", "read_file foo.cpp",
                                         "line one\nline two\tend \\ ok", "");
    char *line2 = edc_chat_bubble_encode("reply", 1, "", "", "hello", "");
    char ins[2048];
    snprintf(ins, sizeof ins,
             "INSERT INTO chat_bubbles(session_id,seq,line) VALUES('s1',1,'%s')", line1 ? line1 : "");
    CHECK(exec_ok(db, ins), "fixture: first legacy bubble row");
    snprintf(ins, sizeof ins,
             "INSERT INTO chat_bubbles(session_id,seq,line) VALUES('s1',2,'%s')", line2 ? line2 : "");
    CHECK(exec_ok(db, ins), "fixture: second legacy bubble row");
    free(line1); free(line2);
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 18, err, sizeof err), "v18 migrates to v19");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
    CHECK(column_exists(db, "chat_bubbles", "highlight_lang") &&
              !column_exists(db, "chat_bubbles", "line"), "line column replaced by typed columns");
    CHECK(query_int64(db, "SELECT count(*) FROM chat_bubbles") == 2, "both rows survive the split");

    char v[512];
    query_text(db, "SELECT kind FROM chat_bubbles WHERE seq=1", v, sizeof v);
    CHECK(strcmp(v, "tool") == 0, "kind migrated");
    query_text(db, "SELECT task_id FROM chat_bubbles WHERE seq=1", v, sizeof v);
    CHECK(strcmp(v, "task-7") == 0, "task_id migrated");
    query_text(db, "SELECT header FROM chat_bubbles WHERE seq=1", v, sizeof v);
    CHECK(strcmp(v, "read_file foo.cpp") == 0, "header migrated");
    query_text(db, "SELECT content FROM chat_bubbles WHERE seq=1", v, sizeof v);
    CHECK(strcmp(v, "line one\nline two\tend \\ ok") == 0, "escaped content decoded byte-for-byte");
    query_text(db, "SELECT highlight_lang FROM chat_bubbles WHERE seq=1", v, sizeof v);
    CHECK(v[0] == '\0', "legacy row has empty highlight_lang");
    CHECK(query_int64(db, "SELECT render_pref FROM chat_bubbles WHERE seq=2") == 1,
          "render_pref migrated");
    sqlite3_close(db);
}

/* The same step against a COMPLETE v17 must be a no-op: idempotence is what lets one
 * migration cover both the broken and the healthy upgrade path. */
static void test_v18_repair_is_idempotent(void)
{
    printf("\n── the v17 -> v18 repair is idempotent on a complete v17 ──\n");
    fresh_dir();
    char path[600];
    db_path(path, sizeof path);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "open a fresh db");
    bool built = exec_ok(db, "PRAGMA journal_mode=WAL") &&
                 exec_ok(db, "PRAGMA foreign_keys=ON") &&
                 exec_ok(db, "CREATE TABLE sessions(session_id INTEGER PRIMARY KEY, "
                             "name TEXT NOT NULL, workdir TEXT NOT NULL DEFAULT '', updated_at INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE groups(session_id INTEGER NOT NULL REFERENCES "
                             "sessions(session_id) ON DELETE CASCADE, group_id INTEGER PRIMARY KEY, "
                             "is_orphaned INTEGER NOT NULL DEFAULT 0, "
                             "is_archived INTEGER NOT NULL DEFAULT 0, "
                             "archived_at INTEGER NOT NULL DEFAULT 0, "
                             "idle_since INTEGER NOT NULL DEFAULT 0)") &&
                 exec_ok(db, "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_entries(entry_id TEXT PRIMARY KEY)") &&
                 exec_ok(db, "CREATE TABLE memo_events(seq INTEGER PRIMARY KEY)") &&
                 exec_ok(db, "INSERT INTO sessions(session_id,name) VALUES(1,'s')") &&
                 exec_ok(db, "INSERT INTO groups(session_id,group_id,idle_since) VALUES(1,10,42)") &&
                 exec_ok(db, "PRAGMA user_version=17");
    CHECK(built, "fixture: a complete v17 database with a persisted idle clock");
    sqlite3_close(db);

    CHECK(sqlite3_open(path, &db) == SQLITE_OK, "reopen the fixture");
    char err[1024] = {0};
    CHECK(edc_migrate_run(db, path, 17, err, sizeof err), "complete v17 migrates to v18");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "user_version is current");
    CHECK(query_int64(db, "SELECT idle_since FROM groups WHERE group_id=10") == 42,
          "existing idle_since is untouched by the idempotent step");
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
    CHECK(!edc_migrate_run(db, path, EDC_DB_VERSION, err, sizeof err), "second run is refused");
    CHECK(strstr(err, "newer EdgeClaw") != NULL,
          "a retry at the current version reports 'nothing to do' as a refusal");
    CHECK(query_int64(db, "PRAGMA user_version") == EDC_DB_VERSION, "still current");
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
    test_real_v12_to_v13();
    test_real_v13_to_v14();
    test_real_v14_to_v15();
    test_real_v15_to_v16();
    test_real_v16_to_v17();
    test_real_v17_repair();
    test_real_v17_repair_all_columns_missing();
    test_v18_repair_is_idempotent();
    test_v18_to_v19_with_chat_bubbles();
    test_rerun_is_refused();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
