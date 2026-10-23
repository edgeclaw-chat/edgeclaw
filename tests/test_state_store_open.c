/* test_state_store_open: the startup contract of the SQLite state store.
 *
 * What this covers, and why each item is worth a test rather than a comment:
 *   - the PRAGMA set actually took effect (a wrong synchronous is invisible until a
 *     crash, so it is asserted here instead);
 *   - the schema is complete and stamped with exactly one version;
 *   - a database from another version is REFUSED (there is no migration path -- that is
 *     the design, so it must not silently half-load);
 *   - a symlinked database is REFUSED (the workspace is model-writable; this exact hole
 *     was exploited once with jobs.json);
 *   - the workspace lock is exclusive (two agent processes would overwrite each other);
 *   - close() checkpoints the WAL away.
 *
 * Uses a temp directory under $TMPDIR. tmpfs is on the local-filesystem whitelist, so the
 * startup check passes there.
 */

#include "edc_config.h"
#include "test_ws.h"
#include "edc_state_store.h"
#include "log.h"
#include "sqlite3.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

static char g_dir[512];

static void reset_dir(void)
{
    char cmd[640];
    edc_state_store_close();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "    (warning: could not reset %s)\n", g_dir);
}

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(g_dir);
    return cfg;
}

static void free_config(struct edc_config *cfg)
{
    if (!cfg)
        return;
    sdsfree(cfg->workspace);
    free(cfg);
}

/* Read one integer PRAGMA from a freshly opened connection to the same file. Opening a
 * second connection for inspection also proves the file is readable by others (it is a
 * normal SQLite database, not something locked away). */
static int inspect_int(const char *path, const char *sql)
{
    sqlite3 *db = NULL;
    int out = -999;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        return -998;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        out = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

static char *inspect_text(const char *path, const char *sql)
{
    sqlite3 *db = NULL;
    char *out = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        return NULL;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
    {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t)
            out = strdup((const char *)t);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* ── 1. opening a fresh workspace ──────────────────────────────────────────── */
static void test_open_creates_schema(void)
{
    printf("\n── open on an empty workspace ──\n");
    reset_dir();

    struct edc_config *cfg = make_config();
    CHECK(edc_state_store_open(cfg), "open succeeds");
    CHECK(edc_state_store_is_open(), "reports open");

    const char *path = edc_state_store_path();
    CHECK(path && strstr(path, "edgeclaw.db") != NULL, "path names edgeclaw.db");
    CHECK(path && file_exists(path), "database file exists");

    /* Idempotent: a second open must not fail or leak a connection. */
    CHECK(edc_state_store_open(cfg), "second open is idempotent");

    free_config(cfg);
}

/* ── 2. the PRAGMA set ─────────────────────────────────────────────────────── */
static void test_pragmas(void)
{
    printf("\n── PRAGMAs ──\n");
    const char *path = edc_state_store_path();

    char *mode = inspect_text(path, "PRAGMA journal_mode");
    CHECK(mode && strcmp(mode, "wal") == 0, "journal_mode is WAL");
    free(mode);

    /* synchronous: 0=OFF 1=NORMAL 2=FULL 3=EXTRA. Asserted on the STORE's connection --
     * these settings are per-connection, so a second connection would only show its own
     * defaults (which cost me one debugging round: it reported 2). This is the decision
     * recorded in the storage design: NORMAL, because FULL measures
     * ~6.5 ms per commit here and would break the < 5 ms target, while NORMAL still
     * survives kill -9. */
    const int sync = edc_state_store_pragma_int("synchronous");
    printf("        (store's PRAGMA synchronous = %d; 1 means NORMAL)\n", sync);
    CHECK(sync == 1, "synchronous is NORMAL (1) on the store connection");

    CHECK(edc_state_store_pragma_int("foreign_keys") == 1,
          "foreign_keys is ON on the store connection");
    CHECK(edc_state_store_pragma_int("trusted_schema") == 0,
          "trusted_schema is OFF on the store connection");
    CHECK(edc_state_store_pragma_int("busy_timeout") == 5000,
          "busy_timeout is 5000 on the store connection");
    CHECK(edc_state_store_pragma_int("wal_autocheckpoint") == 1000,
          "wal_autocheckpoint is 1000 on the store connection");

    /* user_version is a property of the FILE, so either connection may read it. */
    CHECK(inspect_int(path, "PRAGMA user_version") == EDC_DB_VERSION,
          "user_version is EDC_DB_VERSION");

    /* The name is interpolated into SQL; a non-identifier must be refused, not passed
     * through. This is what keeps the accessor from becoming an injection point. */
    CHECK(edc_state_store_pragma_int("synchronous; DROP TABLE tasks") == -1,
          "a pragma name that is not a bare identifier is refused");
}

/* ── 3. every expected table exists ────────────────────────────────────────── */
static void test_schema_tables(void)
{
    printf("\n── schema ──\n");
    const char *path = edc_state_store_path();
    const char *tables[] = {"sessions", "groups", "group_fold_files", "tasks", "task_deps",
                            "task_tool_calls", "turns", "messages", "memo_entries",
                            "memo_events", NULL};

    for (int i = 0; tables[i]; i++)
    {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s'", tables[i]);
        const int n = inspect_int(path, sql);
        char msg[160];
        snprintf(msg, sizeof(msg), "table %s exists", tables[i]);
        CHECK(n == 1, msg);
    }

    /* The messages table is WITHOUT ROWID and keyed by (task_id, turn_ord, msg_ord). */
    CHECK(inspect_int(path,
                      "SELECT count(*) FROM pragma_index_list('messages') WHERE name IS NOT NULL") >= 1,
          "messages has the mid index");

    /* Both message indexes: mid is present, task_id is deliberately ABSENT.
     * messages(task_id) is not needed -- the PRIMARY KEY index already answers
     * `count(*) ... WHERE task_id=?` as a COVERING INDEX, without reading `json`.
     * (An earlier version of this table was WITHOUT ROWID, where that index was actively
     * harmful; see the long comment on the DDL. On a rowid table it is merely redundant.) */
    CHECK(inspect_int(path,
                      "SELECT count(*) FROM sqlite_master WHERE type='index' "
                      "AND name='messages_by_mid'") == 1,
          "messages_by_mid exists");
    CHECK(inspect_int(path,
                      "SELECT count(*) FROM sqlite_master WHERE type='index' "
                      "AND name='messages_by_task'") == 0,
          "no redundant messages(task_id) index (the PK index covers counts)");

    /* The messages table is a ROWID table, not WITHOUT ROWID -- a measured 44% difference
     * on the load path (see the DDL comment). pragma_table_list().wr is 1 for WITHOUT
     * ROWID tables and 0 otherwise, which asserts the property directly. */
    CHECK(inspect_int(path,
                      "SELECT wr FROM pragma_table_list WHERE name='messages'") == 0,
          "messages is a rowid table (not WITHOUT ROWID) -- 44% faster to load");

    /* id is the identity, and the positional key keeps its own index. Losing either one is
     * a silent correctness/perf regression: without the index the loader full-scans (a
     * measured 400x), and without the unique id a message has no name of its own. */
    CHECK(inspect_int(path,
                      "SELECT count(*) FROM pragma_table_info('messages') WHERE name='id'") == 1,
          "messages has an id column");
    CHECK(inspect_int(path,
                      "SELECT pk FROM pragma_table_info('messages') WHERE name='id'") == 1,
          "id is the primary key");
    CHECK(inspect_int(path,
                      "SELECT count(*) FROM sqlite_master WHERE type='index' "
                      "AND name='messages_natural'") == 1,
          "the positional key has its own unique index (else the loader full-scans)");
    CHECK(inspect_int(path,
                      "SELECT count(*) FROM pragma_index_info('messages_natural')") == 3,
          "messages_natural covers (task_id, turn_ord, msg_ord)");

    /* Every timestamp column is INTEGER seconds -- no TEXT dates, no mixed units. */
    {
        const char *stamp_cols[] = {"sessions.created_at", "sessions.last_active",
                                    "messages.ts", NULL};
        for (int i = 0; stamp_cols[i]; i++)
        {
            char tbl[32], col[32], sql[192], msg[128];
            sscanf(stamp_cols[i], "%31[^.].%31s", tbl, col);
            snprintf(sql, sizeof(sql),
                     "SELECT type FROM pragma_table_info('%s') WHERE name='%s'", tbl, col);
            char *type = inspect_text(path, sql);
            snprintf(msg, sizeof(msg), "%s is INTEGER, not TEXT (%s)", stamp_cols[i],
                     type ? type : "missing");
            CHECK(type && strcmp(type, "INTEGER") == 0, msg);
            free(type);
        }
    }
}

/* ── 4. a different schema version is refused ──────────────────────────────── */
static void test_version_mismatch_refused(void)
{
    printf("\n── version mismatch ──\n");
    edc_state_store_close();
    reset_dir();

    const char *path_in_ws = NULL;
    (void)path_in_ws;
    {
        struct edc_config *cfg = make_config();
        /* Create a database behind the store's back, stamped with a future version. */
        char path[600];
        snprintf(path, sizeof(path), "%s/edgeclaw.db", g_dir);
        sqlite3 *db = NULL;
        if (sqlite3_open(path, &db) == SQLITE_OK)
        {
            sqlite3_exec(db, "CREATE TABLE junk(x)", NULL, NULL, NULL);
            sqlite3_exec(db, "PRAGMA user_version=99", NULL, NULL, NULL);
            sqlite3_close(db);
        }

        CHECK(!edc_state_store_open(cfg), "open refuses a v99 database");
        CHECK(!edc_state_store_is_open(), "store stays closed after refusal");
        free_config(cfg);
    }

    /* And a clean workspace still opens afterwards (the refusal is not sticky). */
    reset_dir();
    struct edc_config *cfg = make_config();
    CHECK(edc_state_store_open(cfg), "a fresh database opens after the refusal");
    free_config(cfg);
}

/* ── 5. a symlinked database is refused ────────────────────────────────────── */
static void test_symlink_refused(void)
{
    printf("\n── symlinked database ──\n");
    edc_state_store_close();
    reset_dir();

    char target[600], link[600];
    snprintf(target, sizeof(target), "%s/elsewhere.db", g_dir);
    snprintf(link, sizeof(link), "%s/edgeclaw.db", g_dir);

    /* A decoy file for the link to point at, so the refusal is about the link and not
     * about a missing target. */
    FILE *f = fopen(target, "w");
    if (f)
        fclose(f);
    if (symlink(target, link) != 0)
    {
        printf("    (skipped: cannot create a symlink here)\n");
        return;
    }

    struct edc_config *cfg = make_config();
    CHECK(!edc_state_store_open(cfg), "open refuses a symlinked edgeclaw.db");
    free_config(cfg);
    unlink(link);
}

/* ── 6. the workspace lock is exclusive ────────────────────────────────────── */
static void test_lock_is_exclusive(void)
{
    printf("\n── workspace lock ──\n");
    edc_state_store_close();
    reset_dir();

    struct edc_config *cfg = make_config();
    CHECK(edc_state_store_open(cfg), "open succeeds");

    /* The store holds LOCK_EX on the database file. A second, independent open file
     * description (flock is per description, so this really tests the lock) must not get
     * it. This is what a second agent process would attempt. */
    const char *path = edc_state_store_path();
    const int fd = open(path, O_RDWR);
    CHECK(fd >= 0, "can open the file for the lock probe");
    if (fd >= 0)
    {
        const int got = flock(fd, LOCK_EX | LOCK_NB);
        CHECK(got != 0, "a second locker is refused while the store holds the lock");
        close(fd);
    }

    free_config(cfg);
}

/* ── 7. close() folds the WAL away ─────────────────────────────────────────── */
static void test_close_checkpoints(void)
{
    printf("\n── close ──\n");
    const char *path = edc_state_store_path();
    char wal[600];
    snprintf(wal, sizeof(wal), "%s-wal", path);

    edc_state_store_close();
    CHECK(!edc_state_store_is_open(), "reports closed");
    CHECK(!file_exists(wal), "close() checkpointed the -wal away");

    /* The lock must be gone too: a fresh open of the same workspace has to succeed. */
    struct edc_config *cfg = make_config();
    CHECK(edc_state_store_open(cfg), "reopen after close succeeds");
    free_config(cfg);
    edc_state_store_close();
}

int main(void)
{
    edc_test_workspace(); /* disposable store for anything that writes state */
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_state_store_%ld",
             (tmp && *tmp) ? tmp : "/tmp", (long)getpid());

    printf("workspace: %s\n", g_dir);

    test_open_creates_schema();
    test_pragmas();
    test_schema_tables();
    test_version_mismatch_refused();
    test_symlink_refused();
    test_lock_is_exclusive();
    test_close_checkpoints();

    edc_state_store_close();
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", g_dir);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
