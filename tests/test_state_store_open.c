/* test_state_store_open: the startup contract of the SQLite state store.
 *
 * What this covers, and why each item is worth a test rather than a comment:
 *   - the PRAGMA set actually took effect (a wrong synchronous is invisible until a
 *     crash, so it is asserted here instead);
 *   - the schema is complete and stamped with exactly one version;
 *   - a database from a NEWER version is REFUSED (this binary must not write a schema it
 *     cannot read). An OLDER version is migrated forward instead, not refused -- that is
 *     test_state_migrate's subject, and it is the path a user upgrading EdgeClaw takes;
 *   - a symlinked database is REFUSED (the workspace is model-writable; this exact hole
 *     was exploited once with jobs.json);
 *   - the workspace lock is exclusive and released on close (two agent processes would
 *     overwrite each other; a stale lock would make the next start fail forever);
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

#include <inttypes.h> /* PRId64 */
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

static void reset_dir(void)
{
    char cmd[640];
    edc_state_store_close();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (test_sh(cmd) != 0)
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

/* Run one read-only query against the store and return its first column.
 *
 * NOT a second connection: the store takes the database exclusively (see the Locking note
 * in edc_state_store.c), so while an agent runs the file is genuinely closed to anything
 * else -- a second connection here would only prove that we get SQLITE_BUSY. Inspection
 * goes through the store's own handle instead, which still reads ROWS (never the
 * in-memory tree), so these assertions stay assertions about what is on disk. */
static int inspect_int(const char *sql)
{
    sqlite3 *db = edc_state_store_handle();
    if (!db)
        return -998;
    int out = -999;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        out = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return out;
}

static char *inspect_text(const char *sql)
{
    sqlite3 *db = edc_state_store_handle();
    if (!db)
        return NULL;
    char *out = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
    {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t)
            out = strdup((const char *)t);
    }
    sqlite3_finalize(st);
    return out;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Capturing the store's own error lines.
 *
 * Two of the refusals below differ ONLY in their sentence -- "another edgeclaw instance
 * is using this workspace" vs "failed quick_check ... keep the -wal file for salvage" --
 * and telling those apart is the whole reason claim_exclusive() looks at the error code
 * instead of reporting a bare busy. Asserting just `open() == false` would pass even if
 * both cases produced the wrong sentence, so the message is captured and checked. */
static char g_log[4096];

static void capture_log(log_Event *ev)
{
    if (ev->level != LOG_ERROR)
        return;
    const size_t used = strlen(g_log);
    if (used + 1 >= sizeof(g_log))
        return;
    vsnprintf(g_log + used, sizeof(g_log) - used, ev->fmt, ev->ap);
    const size_t n = strlen(g_log);
    if (n + 1 < sizeof(g_log))
    {
        g_log[n] = '\n';
        g_log[n + 1] = '\0';
    }
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

    char *mode = inspect_text("PRAGMA journal_mode");
    CHECK(mode && strcmp(mode, "wal") == 0, "journal_mode is WAL");
    free(mode);

    /* The one-process-per-workspace guarantee, taken from SQLite instead of a second lock
     * (see the Locking note in edc_state_store.c). Asserted here because a silently
     * ignored pragma would mean no exclusion at all. */
    char *locking = inspect_text("PRAGMA locking_mode");
    CHECK(locking && strcmp(locking, "exclusive") == 0, "locking_mode is EXCLUSIVE");
    free(locking);

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

    /* user_version is a property of the FILE, not of a connection. */
    CHECK(inspect_int("PRAGMA user_version") == EDC_DB_VERSION,
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
    const char *tables[] = {"sessions", "groups", "group_fold_files", "tasks", "task_deps",
                            "task_tool_calls", "turns", "messages", "memo_entries",
                            "memo_events", NULL};

    for (int i = 0; tables[i]; i++)
    {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s'", tables[i]);
        const int n = inspect_int(sql);
        char msg[160];
        snprintf(msg, sizeof(msg), "table %s exists", tables[i]);
        CHECK(n == 1, msg);
    }

    /* The messages table is WITHOUT ROWID and keyed by (task_id, turn_ord, msg_ord). */
    CHECK(inspect_int("SELECT count(*) FROM pragma_index_list('messages') WHERE name IS NOT NULL") >= 1,
          "messages has the mid index");

    /* Both message indexes: mid is present, task_id is deliberately ABSENT.
     * messages(task_id) is not needed -- the PRIMARY KEY index already answers
     * `count(*) ... WHERE task_id=?` as a COVERING INDEX, without reading `json`.
     * (An earlier version of this table was WITHOUT ROWID, where that index was actively
     * harmful; see the long comment on the DDL. On a rowid table it is merely redundant.) */
    CHECK(inspect_int("SELECT count(*) FROM sqlite_master WHERE type='index' "
                      "AND name='messages_by_mid'") == 1,
          "messages_by_mid exists");
    CHECK(inspect_int("SELECT count(*) FROM sqlite_master WHERE type='index' "
                      "AND name='messages_by_task'") == 0,
          "no redundant messages(task_id) index (the PK index covers counts)");

    /* The messages table is a ROWID table, not WITHOUT ROWID -- a measured 44% difference
     * on the load path (see the DDL comment). pragma_table_list().wr is 1 for WITHOUT
     * ROWID tables and 0 otherwise, which asserts the property directly. */
    CHECK(inspect_int("SELECT wr FROM pragma_table_list WHERE name='messages'") == 0,
          "messages is a rowid table (not WITHOUT ROWID) -- 44% faster to load");

    /* id is the identity, and the positional key keeps its own index. Losing either one is
     * a silent correctness/perf regression: without the index the loader full-scans (a
     * measured 400x), and without the unique id a message has no name of its own. */
    CHECK(inspect_int("SELECT count(*) FROM pragma_table_info('messages') WHERE name='id'") == 1,
          "messages has an id column");
    CHECK(inspect_int("SELECT pk FROM pragma_table_info('messages') WHERE name='id'") == 1,
          "id is the primary key");
    CHECK(inspect_int("SELECT count(*) FROM sqlite_master WHERE type='index' "
                      "AND name='messages_natural'") == 1,
          "the positional key has its own unique index (else the loader full-scans)");
    CHECK(inspect_int("SELECT count(*) FROM pragma_index_info('messages_natural')") == 3,
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
            char *type = inspect_text(sql);
            snprintf(msg, sizeof(msg), "%s is INTEGER, not TEXT (%s)", stamp_cols[i],
                     type ? type : "missing");
            CHECK(type && strcmp(type, "INTEGER") == 0, msg);
            free(type);
        }
    }
}

/* ── 4. a NEWER schema version is refused ──────────────────────────────────── */
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

    /* The store takes the database EXCLUSIVELY (locking_mode=EXCLUSIVE). A second
     * connection -- what a second agent process would attempt, and equally what a second
     * connection in this one would attempt -- must not get in. Both a reader and a writer
     * are refused: the guarantee is about the whole database, not about a writer lock. */
    /* Copied, not borrowed: edc_state_store_close() does `sdsfree(g_db_path)`, so a pointer
     * from edc_state_store_path() DIES with the store -- and this test uses the path after
     * closing it (the "lock is released" assertion below). The borrowed pointer there read
     * freed memory, which happened to still spell the path, so the test only ever passed by
     * luck; a clean rebuild was enough to change what the freed bytes held. */
    char path[1024];
    snprintf(path, sizeof(path), "%s", edc_state_store_path());
    const struct
    {
        int flags;
        const char *what;
    } kAttempts[] = {{SQLITE_OPEN_READONLY, "a second read-only connection"},
                     {SQLITE_OPEN_READWRITE, "a second read-write connection"}};
    for (size_t i = 0; i < sizeof(kAttempts) / sizeof(kAttempts[0]); i++)
    {
        sqlite3 *other = NULL;
        const int rc = sqlite3_open_v2(path, &other, kAttempts[i].flags, NULL);
        int busy = rc;
        if (rc == SQLITE_OK)
        {
            sqlite3_stmt *st = NULL;
            busy = sqlite3_prepare_v2(other, "SELECT count(*) FROM sessions", -1, &st, NULL);
            if (busy == SQLITE_OK)
                busy = sqlite3_step(st);
            sqlite3_finalize(st);
        }
        sqlite3_close(other);
        char msg[160];
        snprintf(msg, sizeof(msg), "%s is refused with SQLITE_BUSY (%d)",
                 kAttempts[i].what, busy);
        CHECK(busy == SQLITE_BUSY || busy == SQLITE_LOCKED, msg);
    }

    /* And closing releases it: a stale lock would make the next start fail forever. */
    edc_state_store_close();
    sqlite3 *after = NULL;
    CHECK(sqlite3_open_v2(path, &after, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK,
          "one read-only connection is allowed once the store is closed");
    sqlite3_close(after);

    free_config(cfg);
}

/* ── 7. another instance holds the workspace ───────────────────────────────── */
static void test_busy_refused(void)
{
    printf("\n── another instance holds the workspace ──\n");
    edc_state_store_close();
    reset_dir();

    /* Build a real database first: the case under test is a second instance meeting a
     * VALID database that somebody else has taken, not a missing file. */
    struct edc_config *cfg = make_config();
    CHECK(edc_state_store_open(cfg), "first open builds the database");

    char path[600];
    snprintf(path, sizeof(path), "%s", edc_state_store_path() ? edc_state_store_path() : "");
    edc_state_store_close();

    /* Stand in for the other agent process. It takes the lock the same way the store
     * does, because locking_mode=EXCLUSIVE is only a request -- the read below is what
     * actually takes it. */
    sqlite3 *other = NULL;
    CHECK(sqlite3_open_v2(path, &other, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK,
          "the stand-in instance opens the database");
    char *err = NULL;
    const int mode_rc = sqlite3_exec(other, "PRAGMA locking_mode=EXCLUSIVE", NULL, NULL, &err);
    sqlite3_free(err);
    err = NULL;
    const int lock_rc = sqlite3_exec(other, "SELECT count(*) FROM sqlite_master", NULL, NULL, &err);
    sqlite3_free(err);
    CHECK(mode_rc == SQLITE_OK && lock_rc == SQLITE_OK, "the stand-in takes the exclusive lock");

    g_log[0] = '\0';
    CHECK(!edc_state_store_open(cfg), "open refuses while another instance holds the database");
    CHECK(!edc_state_store_is_open(), "store stays closed after the refusal");
    printf("    log: %s", g_log);
    CHECK(strstr(g_log, "another edgeclaw instance") != NULL,
          "the refusal names the other instance instead of surfacing a bare busy");

    sqlite3_close(other);

    /* Not sticky: the same workspace opens once the other instance is gone. */
    g_log[0] = '\0';
    CHECK(edc_state_store_open(cfg), "open succeeds once the other instance is gone");

    free_config(cfg);
}

/* ── 8. a corrupt database is refused ──────────────────────────────────────── */
static void test_corrupt_refused(void)
{
    printf("\n── corrupt database ──\n");
    edc_state_store_close();
    reset_dir();

    char path[600], wal[640];
    snprintf(path, sizeof(path), "%s/edgeclaw.db", g_dir);
    snprintf(wal, sizeof(wal), "%s-wal", path);

    /* Deliberately not a zero-length file: SQLite treats that as a valid empty database,
     * so the garbage has to be real bytes to reach the corruption path at all. */
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        printf("    (skipped: cannot write %s)\n", path);
        return;
    }
    for (int i = 0; i < 4096; i++)
        fputc(0xA5, f);
    fclose(f);

    /* A stale -wal, the state a crashed run leaves behind. It is planted so the refusal is
     * exercised with one present, but it is NOT asserted on afterwards: measured here,
     * SQLite's own recovery discards a WAL it cannot use against a file that is not a
     * database, so its fate is SQLite's call and not something this code promises. */
    f = fopen(wal, "wb");
    if (f)
    {
        fputs("not really a wal", f);
        fclose(f);
    }

    struct edc_config *cfg = make_config();
    g_log[0] = '\0';
    CHECK(!edc_state_store_open(cfg), "open refuses a corrupt database");
    CHECK(!edc_state_store_is_open(), "store stays closed after the refusal");
    printf("    log: %s", g_log);
    CHECK(strstr(g_log, "another edgeclaw instance") == NULL,
          "a corrupt file is not reported as another instance");
    /* The corrupt file itself must survive: it is the evidence salvage works from, so a
     * refusal must not tidy it away. (The -wal is NOT asserted here: SQLite's own recovery
     * discards a WAL it cannot use against a file that is not a database, so whether it
     * survives is SQLite's call, not this code's -- see the note in the handover doc.) */
    CHECK(file_exists(path), "the damaged database is kept, not deleted");
    free_config(cfg);

    /* And a clean workspace still opens afterwards (the refusal is not sticky). */
    reset_dir();
    cfg = make_config();
    CHECK(edc_state_store_open(cfg), "a fresh database opens after the refusal");
    free_config(cfg);
}

/* ── 9. close() folds the WAL away ─────────────────────────────────────────── */
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
    /* Every message this file asserts on is a log_error from the store. */
    log_add_callback(capture_log, NULL, LOG_TRACE);
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_test_state_store_%" PRId64,
             (tmp && *tmp) ? tmp : "/tmp", (int64_t)getpid());

    printf("workspace: %s\n", g_dir);

    test_open_creates_schema();
    test_pragmas();
    test_schema_tables();
    test_version_mismatch_refused();
    test_symlink_refused();
    test_lock_is_exclusive();
    test_busy_refused();
    test_corrupt_refused();
    test_close_checkpoints();

    edc_state_store_close();
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "(warning: could not clean %s)\n", g_dir);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
