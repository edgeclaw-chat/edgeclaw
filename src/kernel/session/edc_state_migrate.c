/* ── Schema migration, in the binary ─────────────────────────────────────────
 *
 * The python scripts under tools/ proved this discipline on real databases. This file is
 * that discipline with a different operator: it runs when the user starts EdgeClaw, so
 * "upgrade" does not mean "find a script, find a python, get the TMPDIR right, and hope".
 *
 * What is deliberately NOT here: a downgrade path, a dual-write mode, or a fallback to
 * some older interpreter of the file. A database is either at the version this binary
 * understands or it is refused with an explanation (see edc_state_store.c).
 */

#include "edc_state_migrate.h"

#include "edc_compat.h"
#include "edc_state_store.h"
#include "edc_version.h"
#include "log.h"
#include "sds.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#ifdef _WIN32
#include <io.h>
#define edc_unlink _unlink
#else
#include <unistd.h>
#define edc_unlink unlink
#endif

/* ══ Small helpers ═══════════════════════════════════════════════════════════════════ */

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || cap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* Run a statement that returns no rows. `what` names it in the error. */
static bool run_sql(sqlite3 *db, const char *sql, const char *what, char *err, size_t cap)
{
    char *msg = NULL;
    const int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
    if (rc != SQLITE_OK)
    {
        set_err(err, cap, "%s failed: %s", what, msg ? msg : sqlite3_errmsg(db));
        sqlite3_free(msg);
        return false;
    }
    sqlite3_free(msg);
    return true;
}

/* Run a query whose first column of the first row is an integer. */
static bool query_one_int(sqlite3 *db, const char *sql, sqlite3_int64 *out, char *err,
                          size_t cap)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
    {
        set_err(err, cap, "prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    const int rc = sqlite3_step(st);
    bool ok = false;
    if (rc == SQLITE_ROW)
    {
        *out = sqlite3_column_int64(st, 0);
        ok = true;
    }
    else if (rc != SQLITE_DONE)
    {
        set_err(err, cap, "query failed: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return ok;
}

/* Run a query whose first column of the first row is TEXT ("ok" / "row counts" / ...). */
static bool query_one_text(sqlite3 *db, const char *sql, char *out, size_t cap, char *err,
                           size_t err_cap)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
    {
        set_err(err, err_cap, "prepare failed: %s", sqlite3_errmsg(db));
        return false;
    }
    const int rc = sqlite3_step(st);
    bool ok = false;
    if (rc == SQLITE_ROW)
    {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        snprintf(out, cap, "%s", t ? t : "");
        ok = true;
    }
    else if (rc != SQLITE_DONE)
    {
        set_err(err, err_cap, "query failed: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return ok;
}

static bool has_column(sqlite3 *db, const char *table, const char *column)
{
    sds sql = sdscatprintf(sdsempty(), "SELECT count(*) FROM pragma_table_info('%s') "
                                       "WHERE name='%s'",
                           table, column);
    sqlite3_int64 n = 0;
    char err[256];
    const bool ok = query_one_int(db, sql, &n, err, sizeof err);
    sdsfree(sql);
    return ok && n == 1;
}

/* Is |column| declared NOT NULL on |table|? pragma_table_info.notnull is 1 for a
 * NOT NULL column. Returns false when the column is absent or the query fails. */
static bool column_is_notnull(sqlite3 *db, const char *table, const char *column,
                              char *err, size_t cap)
{
    /* "notnull" is a keyword in SQLite: quote it or the prepare fails with
     * 'near "notnull": syntax error'. */
    sds sql = sdscatprintf(sdsempty(), "SELECT \"notnull\" FROM pragma_table_info('%s') "
                                       "WHERE name='%s'",
                           table, column);
    sqlite3_int64 n = 0;
    const bool ok = query_one_int(db, sql, &n, err, cap) && n == 1;
    sdsfree(sql);
    return ok;
}

/* ══ Backup ════════════════════════════════════════════════════════════════════════ */

/* Copy one file. On failure the partial destination is removed: a truncated copy that
 * looks like a backup is worse than no backup, because the next thing that happens is
 * someone trusting it. */
static bool copy_file(const char *src, const char *dst, char *err, size_t cap)
{
    FILE *in = fopen(src, "rb");
    if (!in)
    {
        set_err(err, cap, "cannot open %s: %s", src, strerror(errno));
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (!out)
    {
        set_err(err, cap, "cannot create %s: %s", dst, strerror(errno));
        fclose(in);
        return false;
    }

    bool ok = true;
    char buf[64 * 1024];
    for (;;)
    {
        const size_t n = fread(buf, 1, sizeof buf, in);
        if (n > 0 && fwrite(buf, 1, n, out) != n)
        {
            set_err(err, cap, "cannot write %s: %s", dst, strerror(errno));
            ok = false;
            break;
        }
        if (n < sizeof buf)
        {
            if (ferror(in))
            {
                set_err(err, cap, "cannot read %s: %s", src, strerror(errno));
                ok = false;
            }
            break;
        }
    }

    if (ok && fflush(out) != 0)
    {
        set_err(err, cap, "cannot flush %s: %s", dst, strerror(errno));
        ok = false;
    }
    if (fclose(out) != 0 && ok)
    {
        set_err(err, cap, "cannot close %s: %s", dst, strerror(errno));
        ok = false;
    }
    fclose(in);

    if (!ok)
        edc_unlink(dst);
    return ok;
}

/* Does `path` open as a SQLite database whose quick_check passes? Used to prove the copy
 * is readable BEFORE anything is written to the original. */
static bool file_is_healthy_db(const char *path, int *task_rows, char *err, size_t cap)
{
    sqlite3 *db = NULL;
    /* Read-only: a diagnostic must not create a -wal or repair anything. */
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
        set_err(err, cap, "cannot open %s: %s", path, db ? sqlite3_errmsg(db) : "out of memory");
        if (db)
            sqlite3_close(db);
        return false;
    }

    char check[128] = {0};
    char qerr[256] = {0};
    bool ok = query_one_text(db, "PRAGMA quick_check(1)", check, sizeof check, qerr, sizeof qerr);
    if (ok && strcmp(check, "ok") != 0)
    {
        set_err(err, cap, "%s failed quick_check: %s", path, check);
        ok = false;
    }
    if (ok && task_rows)
    {
        sqlite3_int64 n = 0;
        if (query_one_int(db, "SELECT count(*) FROM tasks", &n, qerr, sizeof qerr))
            *task_rows = (int)n;
    }
    sqlite3_close(db);
    return ok;
}

/* Fold the WAL back into the main file, then copy it aside.
 *
 * The checkpoint is what makes "one file" a complete backup: without it the copy could
 * describe a database missing its most recent commits, and the -wal would have to be
 * copied too (which the older python scripts did, and could still be mis-assembled by
 * hand). After TRUNCATE the main file is the whole database, so the common case needs no
 * sibling files at all. The siblings are still copied when they exist, because a
 * checkpoint can legitimately refuse (a reader holding an old snapshot) and in that case
 * the -wal IS part of the data.
 */
static bool backup_database(sqlite3 *db, const char *db_path, int from, sds *out_path,
                           char *err, size_t cap)
{
    if (!run_sql(db, "PRAGMA wal_checkpoint(TRUNCATE)", "wal checkpoint", err, cap))
    {
        /* Not fatal: the copy below includes -wal, which is where the data then lives. */
        log_warn("state_store: wal checkpoint before backup failed (%s); copying -wal too", err);
    }

    struct tm tm_now;
    const time_t now = time(NULL);
    char stamp[32] = "unknown";
    if (edc_localtime_r(&now, &tm_now) && strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm_now) == 0)
        snprintf(stamp, sizeof stamp, "unknown");

    sds dst = sdscatprintf(sdsempty(), "%s.v%d.%s", db_path, from, stamp);
    if (!copy_file(db_path, dst, err, cap))
    {
        sdsfree(dst);
        return false;
    }

    const char *suffixes[] = {"-wal", "-shm"};
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++)
    {
        sds src = sdscatprintf(sdsempty(), "%s%s", db_path, suffixes[i]);
        FILE *probe = fopen(src, "rb");
        const bool present = probe != NULL;
        if (probe)
            fclose(probe);
        if (present)
        {
            sds side = sdscatprintf(sdsempty(), "%s%s", dst, suffixes[i]);
            char side_err[256] = {0};
            if (!copy_file(src, side, side_err, sizeof side_err))
            {
                /* Only the main file is load-bearing; a missing -shm is rebuilt by SQLite
                 * and a missing -wal after a successful TRUNCATE holds nothing. Say so and
                 * keep going rather than aborting a migration over a temp file. */
                log_warn("state_store: could not copy %s (%s)", src, side_err);
                edc_unlink(side);
            }
            sdsfree(side);
        }
        sdsfree(src);
    }

    int rows = 0;
    if (!file_is_healthy_db(dst, &rows, err, cap))
    {
        sdsfree(dst);
        return false;
    }

    log_info("state_store: backup written to %s (%d task rows, quick_check ok)", dst, rows);
    if (out_path)
        *out_path = dst;
    else
        sdsfree(dst);
    return true;
}

/* ══ Verification ══════════════════════════════════════════════════════════════════ */

/* Row counts that a migration must not change, captured before the first step. */
struct row_baseline
{
    const char *table;
    sqlite3_int64 before;
};

static bool capture_row_counts(sqlite3 *db, struct row_baseline *rows, size_t n, char *err,
                               size_t cap)
{
    for (size_t i = 0; i < n; i++)
    {
        sds sql = sdscatprintf(sdsempty(), "SELECT count(*) FROM %s", rows[i].table);
        char qerr[256] = {0};
        const bool ok = query_one_int(db, sql, &rows[i].before, qerr, sizeof qerr);
        sdsfree(sql);
        if (!ok)
        {
            set_err(err, cap, "cannot count %s: %s", rows[i].table, qerr);
            return false;
        }
    }
    return true;
}

/* ══ The steps ═════════════════════════════════════════════════════════════════════ */

/* v7 -> v8. Two columns that used to be implicit became explicit state.
 *
 *   tasks.memo_baseline_done   was inferred from `memo_seen_seq == 0`, which overloaded one
 *                              watermark with two unrelated facts, so the case that matters
 *                              most (the index was delivered while memory was still empty)
 *                              was unrepresentable.
 *   memo_events.changed        was in-memory only, so a restart turned every past change
 *                              into "unknown" and notices fell back to reprinting whole
 *                              summaries -- the exact rendering the mask exists to avoid.
 *
 * ADD COLUMN with a constant default is O(1) in SQLite (no table rebuild), which is why
 * this stays a handful of statements on a 462 MB database.
 *
 * The backfill is not a guess: under v7 every notice delivery advanced `memo_seen_seq`, so
 * a watermark past the origin means the index was necessarily delivered. `changed` stays 0
 * -- the documented "unknown", which renders exactly as v7 did. */
static bool step_v7_to_v8(sqlite3 *db, char *err, size_t cap)
{
    if (!run_sql(db, "ALTER TABLE tasks ADD COLUMN memo_baseline_done INTEGER NOT NULL DEFAULT 0",
                 "add tasks.memo_baseline_done", err, cap))
        return false;
    if (!run_sql(db, "ALTER TABLE memo_events ADD COLUMN changed INTEGER NOT NULL DEFAULT 0",
                 "add memo_events.changed", err, cap))
        return false;
    if (!run_sql(db,
                 "UPDATE tasks SET memo_baseline_done = "
                 "CASE WHEN memo_seen_seq > 0 THEN 1 ELSE 0 END",
                 "backfill memo_baseline_done", err, cap))
        return false;
    return true;
}

/* What v8 claims to have achieved. Run after the transaction committed.
 *
 * The last two assertions are the ones worth having: they are the invariants the python
 * script established on real data, and the failure they guard is silent (a task marked
 * un-baselined while its watermark says otherwise gets sent an index it has already seen). */
static bool verify_v8(sqlite3 *db, char *err, size_t cap)
{
    if (!has_column(db, "tasks", "memo_baseline_done"))
    {
        set_err(err, cap, "v8 is stamped but tasks has no memo_baseline_done column");
        return false;
    }
    if (!has_column(db, "memo_events", "changed"))
    {
        set_err(err, cap, "v8 is stamped but memo_events has no changed column");
        return false;
    }

    sqlite3_int64 n = 0;
    if (!query_one_int(db, "SELECT count(*) FROM tasks "
                           "WHERE memo_baseline_done NOT IN (0, 1)",
                       &n, err, cap))
        return false;
    if (n != 0)
    {
        set_err(err, cap, "%lld tasks have a non-boolean memo_baseline_done", (long long)n);
        return false;
    }

    if (!query_one_int(db, "SELECT count(*) FROM tasks "
                           "WHERE memo_seen_seq > 0 AND memo_baseline_done = 0",
                       &n, err, cap))
        return false;
    if (n != 0)
    {
        set_err(err, cap, "%lld tasks are un-baselined but have memo_seen_seq > 0",
                (long long)n);
        return false;
    }
    return true;
}


/* v8 -> v9: chat_bubbles, the TUI's durable view state (was chat.jsonl).
 * Purely additive: no existing table is touched, so a 125 MB v8 database
 * migrates with zero data rewrite. The DDL is duplicated from EDC_SCHEMA_SQL on
 * purpose -- a fresh v9 database and a migrated one must have the same shape,
 * and tests/test_state_migrate.c asserts exactly that. */
static bool step_v8_to_v9(sqlite3 *db, char *err, size_t cap)
{
    if (!run_sql(db,
                 "CREATE TABLE IF NOT EXISTS chat_bubbles ("
                 "  session_id TEXT NOT NULL,"
                 "  seq        INTEGER NOT NULL,"
                 "  line       TEXT NOT NULL,"
                 "  PRIMARY KEY (session_id, seq)"
                 ")",
                 "create chat_bubbles", err, cap))
        return false;
    return run_sql(db,
                   "CREATE INDEX IF NOT EXISTS chat_bubbles_by_session "
                   "ON chat_bubbles(session_id, seq DESC)",
                   "create chat_bubbles_by_session", err, cap);
}

static bool verify_v9(sqlite3 *db, char *err, size_t cap)
{
    sqlite3_int64 n = 0;
    if (!query_one_int(db,
                       "SELECT count(*) FROM sqlite_master "
                       "WHERE type='table' AND name='chat_bubbles'",
                       &n, err, cap))
        return false;
    if (n != 1)
    {
        set_err(err, cap, "v9 is stamped but chat_bubbles is missing");
        return false;
    }
    if (!query_one_int(db,
                       "SELECT count(*) FROM sqlite_master "
                       "WHERE type='index' AND name='chat_bubbles_by_session'",
                       &n, err, cap))
        return false;
    if (n != 1)
    {
        set_err(err, cap, "v9 is stamped but chat_bubbles_by_session is missing");
        return false;
    }
    return true;
}

/* v9 -> v10: persist the reduced memory-round corpus budget on each group.
 * Purely additive (one ALTER TABLE ADD COLUMN, constant default), so a 462 MB v9
 * database migrates with zero data rewrite. 0 is the documented "full cap" value, which
 * is exactly what every pre-v10 group had in memory before the restart. */
static bool step_v9_to_v10(sqlite3 *db, char *err, size_t cap)
{
    return run_sql(db,
                   "ALTER TABLE groups ADD COLUMN fold_corpus_budget INTEGER NOT NULL DEFAULT 0",
                   "add groups.fold_corpus_budget", err, cap);
}

static bool verify_v10(sqlite3 *db, char *err, size_t cap)
{
    if (!has_column(db, "groups", "fold_corpus_budget"))
    {
        set_err(err, cap, "v10 is stamped but groups has no fold_corpus_budget column");
        return false;
    }
    /* The default is load-bearing: 0 means "full cap", not "fold nothing". A migration
     * that wrote any other value would silently shrink every existing group's corpus. */
    sqlite3_int64 n = 0;
    if (!query_one_int(db, "SELECT count(*) FROM groups WHERE fold_corpus_budget != 0", &n, err,
                       cap))
        return false;
    if (n != 0)
    {
        set_err(err, cap, "%lld groups have a non-default fold_corpus_budget after migration",
                (long long)n);
        return false;
    }
    return true;
}

/* v10 -> v11: persist the "fold failed, barrier still held" state on each group.
 * Purely additive (one ALTER TABLE ADD COLUMN, constant default); 0 means healthy,
 * which is exactly what every pre-v11 group was in memory before this change. */
static bool step_v10_to_v11(sqlite3 *db, char *err, size_t cap)
{
    return run_sql(db,
                   "ALTER TABLE groups ADD COLUMN fold_failure_state INTEGER NOT NULL DEFAULT 0",
                   "add groups.fold_failure_state", err, cap) &&
           run_sql(db,
                   "ALTER TABLE groups ADD COLUMN fold_held INTEGER NOT NULL DEFAULT 0",
                   "add groups.fold_held", err, cap);
}

static bool verify_v11(sqlite3 *db, char *err, size_t cap)
{
    if (!has_column(db, "groups", "fold_failure_state") || !has_column(db, "groups", "fold_held"))
    {
        set_err(err, cap, "v11 is stamped but groups is missing a fold-state column");
        return false;
    }
    /* The defaults are load-bearing: any other value would freeze pre-v11 groups whose
     * folds had actually succeeded. */
    sqlite3_int64 n = 0;
    if (!query_one_int(db, "SELECT count(*) FROM groups WHERE fold_failure_state != 0 OR fold_held != 0",
                       &n, err, cap))
        return false;
    if (n != 0)
    {
        set_err(err, cap, "%lld groups have a non-default fold state after migration", (long long)n);
        return false;
    }
    return true;
}

/* v11 -> v12: count how often each memo entry's detail was read via memory_get.
 * Additive like the other bumps: one ALTER TABLE ADD COLUMN with a constant default,
 * so a large v11 DB upgrades with zero data rewrite. 0 is the honest value for every
 * pre-v12 entry -- their reads were never recorded, and inventing a number would
 * corrupt the usage analysis this column exists to enable. */
static bool step_v11_to_v12(sqlite3 *db, char *err, size_t cap)
{
    return run_sql(db, "ALTER TABLE memo_entries ADD COLUMN read_count INTEGER NOT NULL DEFAULT 0",
                   "add memo_entries.read_count", err, cap);
}

static bool verify_v12(sqlite3 *db, char *err, size_t cap)
{
    if (!has_column(db, "memo_entries", "read_count"))
    {
        set_err(err, cap, "v12 is stamped but memo_entries has no read_count column");
        return false;
    }
    sqlite3_int64 n = 0;
    if (!query_one_int(db, "SELECT count(*) FROM memo_entries WHERE read_count != 0", &n, err, cap))
        return false;
    if (n != 0)
    {
        set_err(err, cap, "%lld entries have a non-default read_count after migration", (long long)n);
        return false;
    }
    return true;
}

/* v12 -> v13: split read_count into two independent signals. RENAME keeps every
 * recorded memory_get count as get_count (no data loss, O(1)), and the new
 * search_count starts at 0 (unrecorded history cannot be invented). */
static bool step_v12_to_v13(sqlite3 *db, char *err, size_t cap)
{
    return run_sql(db, "ALTER TABLE memo_entries RENAME COLUMN read_count TO get_count",
                   "rename memo_entries.read_count to get_count", err, cap) &&
           run_sql(db, "ALTER TABLE memo_entries ADD COLUMN search_count INTEGER NOT NULL DEFAULT 0",
                   "add memo_entries.search_count", err, cap);
}

static bool verify_v13(sqlite3 *db, char *err, size_t cap)
{
    if (!has_column(db, "memo_entries", "get_count") || !has_column(db, "memo_entries", "search_count"))
    {
        set_err(err, cap, "v13 is stamped but memo_entries is missing a usage-count column");
        return false;
    }
    if (has_column(db, "memo_entries", "read_count"))
    {
        set_err(err, cap, "v13 is stamped but the old read_count column still exists");
        return false;
    }
    sqlite3_int64 n = 0;
    if (!query_one_int(db, "SELECT count(*) FROM memo_entries WHERE search_count != 0", &n, err, cap))
        return false;
    if (n != 0)
    {
        set_err(err, cap, "%lld entries have a non-default search_count after migration", (long long)n);
        return false;
    }
    return true;
}

/* v13 -> v14: the independent corpus budget for the two-stage extract's L1 summary.
 * Additive O(1) ADD COLUMN; 0 is the honest "full cap" default for every pre-v14 group. */
static bool step_v13_to_v14(sqlite3 *db, char *err, size_t cap)
{
    return run_sql(db, "ALTER TABLE groups ADD COLUMN extract_summary_budget INTEGER NOT NULL DEFAULT 0",
                   "add groups.extract_summary_budget", err, cap);
}

static bool verify_v14(sqlite3 *db, char *err, size_t cap)
{
    if (!has_column(db, "groups", "extract_summary_budget"))
    {
        set_err(err, cap, "v14 is stamped but groups has no extract_summary_budget column");
        return false;
    }
    sqlite3_int64 n = 0;
    if (!query_one_int(db, "SELECT count(*) FROM groups WHERE extract_summary_budget != 0", &n, err, cap))
        return false;
    if (n != 0)
    {
        set_err(err, cap, "%lld groups have a non-default extract_summary_budget after migration",
                (long long)n);
        return false;
    }
    return true;
}

/* v14 -> v15: the `jobs` table (scheduled jobs, replacing workspace/jobs.json).
 * Two DDL statements: the table and its next_run index. The old jobs.json is NOT
 * imported -- those schedules are intentionally dropped. */
static bool step_v14_to_v15(sqlite3 *db, char *err, size_t cap)
{
    return run_sql(db,
                   "CREATE TABLE IF NOT EXISTS jobs ("
                   "  job_id         TEXT PRIMARY KEY,"
                   "  name           TEXT NOT NULL DEFAULT '',"
                   "  message        TEXT NOT NULL DEFAULT '',"
                   "  channel        TEXT NOT NULL DEFAULT '',"
                   "  kind           TEXT NOT NULL,"
                   "  tz             TEXT NOT NULL DEFAULT '',"
                   "  every_seconds  INTEGER NOT NULL DEFAULT 0,"
                   "  cron_expr      TEXT NOT NULL DEFAULT '',"
                   "  next_run_at    INTEGER NOT NULL DEFAULT 0,"
                   "  once           INTEGER NOT NULL DEFAULT 0"
                   ");",
                   "create jobs table", err, cap) &&
           run_sql(db, "CREATE INDEX IF NOT EXISTS jobs_by_next_run ON jobs(next_run_at);",
                   "create jobs_by_next_run index", err, cap);
}

static bool verify_v15(sqlite3 *db, char *err, size_t cap)
{
    if (!has_column(db, "jobs", "job_id") || !has_column(db, "jobs", "next_run_at"))
    {
        set_err(err, cap, "v15 is stamped but jobs table is missing or malformed");
        return false;
    }
    sqlite3_int64 n = 0;
    if (!query_one_int(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='jobs'", &n, err, cap))
        return false;
    if (n != 1)
    {
        set_err(err, cap, "v15: expected exactly one jobs table, found %lld", (long long)n);
        return false;
    }
    return true;
}

/* v15 -> v16: sessions.workdir becomes NOT NULL.
 *
 * A session that used to be "unbound" had workdir = NULL. From v16 on, the unbound state
 * is the empty string: load pins '' to the launch directory (edc_state_store_load), and
 * every writing path persists either a real path or ''. Two statements, both inside the
 * runner's transaction:
 *   1. backfill NULL -> '' (SET NOT NULL refuses a column holding NULL);
 *   2. ALTER COLUMN SET NOT NULL -- supported by SQLite 3.37+ (this build ships 3.53.4),
 *      O(1) metadata change, and unlike a table rebuild it does not rename the table out
 *      from under the child tables' ON DELETE CASCADE foreign keys.
 *
 * No DEFAULT is set here: this SQLite build has no ALTER COLUMN SET DEFAULT syntax. Every
 * INSERT that omits workdir was changed (same commit) to pass '' explicitly, so a migrated
 * database with no column default keeps working. A freshly created database still declares
 * DEFAULT '' in EDC_SCHEMA_SQL as a convenience; the two shapes differ only in that unused
 * default, which no code path relies on. */
static bool step_v15_to_v16(sqlite3 *db, char *err, size_t cap)
{
    return run_sql(db, "UPDATE sessions SET workdir='' WHERE workdir IS NULL",
                   "backfill sessions.workdir NULL -> ''", err, cap) &&
           run_sql(db, "ALTER TABLE sessions ALTER COLUMN workdir SET NOT NULL",
                   "make sessions.workdir NOT NULL", err, cap);
}

static bool verify_v16(sqlite3 *db, char *err, size_t cap)
{
    /* The NOT NULL constraint itself is the invariant: SQLite rejects any NULL workdir,
     * so there is no separate "no NULL rows" check to perform (a row could only have a
     * NULL there if the constraint were absent, which is what this check verifies). */
    return column_is_notnull(db, "sessions", "workdir", err, cap);
}

/* v16 -> v17: group retirement + a persistent idle clock. Three additive O(1) ADD
 * COLUMNs, all defaulting to 0, so every pre-v17 group starts un-archived with an
 * unknown idle clock (which the load path re-derives conservatively). */
static bool step_v16_to_v17(sqlite3 *db, char *err, size_t cap)
{
    return run_sql(db, "ALTER TABLE groups ADD COLUMN is_archived INTEGER NOT NULL DEFAULT 0",
                   "add groups.is_archived", err, cap) &&
           run_sql(db, "ALTER TABLE groups ADD COLUMN archived_at INTEGER NOT NULL DEFAULT 0",
                   "add groups.archived_at", err, cap) &&
           run_sql(db, "ALTER TABLE groups ADD COLUMN idle_since INTEGER NOT NULL DEFAULT 0",
                   "add groups.idle_since", err, cap);
}

static bool verify_v17(sqlite3 *db, char *err, size_t cap)
{
    if (!has_column(db, "groups", "is_archived") || !has_column(db, "groups", "archived_at") ||
        !has_column(db, "groups", "idle_since"))
    {
        set_err(err, cap, "v17 is stamped but groups lacks is_archived / archived_at / idle_since");
        return false;
    }
    sqlite3_int64 n = 0;
    if (!query_one_int(db, "SELECT count(*) FROM groups WHERE is_archived != 0 OR archived_at != 0", &n, err, cap))
        return false;
    if (n != 0)
    {
        set_err(err, cap, "%lld groups have non-default retirement fields after migration", (long long)n);
        return false;
    }
    return true;
}

/* v17 -> v18: repair incomplete v17 databases.
 *
 * Some v17 databases were stamped by an early build of the group-retirement change whose
 * v17 step added only is_archived / archived_at -- idle_since came later in the same
 * release. Those files carry `user_version=17` but lack groups.idle_since, so the current
 * load/flush SQL (which references it) fails to prepare: no group loads and the session
 * is quarantined -- every group vanishes from the sidebar.
 *
 * Because the stamp already says 17, check_or_create_schema() takes the "same version,
 * nothing to do" branch and never re-runs step_v16_to_v17. This step is deliberately
 * IDEMPOTENT: it adds only the columns that are actually missing, so both a broken v17
 * (one or two columns absent) and a complete v17 (all three present) upgrade cleanly.
 * Default 0 matches the original v17 contract (active / unknown idle). */
static bool step_v17_to_v18(sqlite3 *db, char *err, size_t cap)
{
    /* One shape, three names: a broken v17 is missing some suffix of these columns.
     * Fixed statements (no formatting/allocation) keep the repair trivially auditable. */
    static const char *const kAdd[] = {
        "ALTER TABLE groups ADD COLUMN is_archived INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE groups ADD COLUMN archived_at INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE groups ADD COLUMN idle_since INTEGER NOT NULL DEFAULT 0",
    };
    static const char *const kCols[] = {"is_archived", "archived_at", "idle_since"};
    for (size_t i = 0; i < sizeof kCols / sizeof kCols[0]; i++)
        if (!has_column(db, "groups", kCols[i]) &&
            !run_sql(db, kAdd[i], "add retirement column (v18 repair)", err, cap))
            return false;
    return true;
}

static bool verify_v18(sqlite3 *db, char *err, size_t cap)
{
    static const char *const kCols[] = {"is_archived", "archived_at", "idle_since"};
    for (size_t i = 0; i < sizeof kCols / sizeof kCols[0]; i++)
        if (!has_column(db, "groups", kCols[i]))
        {
            set_err(err, cap, "v18 is stamped but groups still lacks a retirement/idle column");
            return false;
        }
    return true;
}

/* ══ The registry ══════════════════════════════════════════════════════════════════
 * Ascending by `from`, terminated by from == 0. Adding a step is the whole cost of a
 * schema bump now; everything else (backup, transaction, stamp, verify) is the runner's.
 *
 * A step here is only half of a bump: EDC_SCHEMA_SQL in edc_state_store.c describes the
 * CURRENT shape and must change in the same commit, or a fresh workspace and a migrated
 * one disagree. tests/test_state_migrate.c asserts that they agree. */
static const struct edc_migration EDC_MIGRATIONS[] = {
    {7, step_v7_to_v8, verify_v8},
    {8, step_v8_to_v9, verify_v9},
    {9, step_v9_to_v10, verify_v10},
    {10, step_v10_to_v11, verify_v11},
    {11, step_v11_to_v12, verify_v12},
    {12, step_v12_to_v13, verify_v13},
    {13, step_v13_to_v14, verify_v14},
    {14, step_v14_to_v15, verify_v15},
    {15, step_v15_to_v16, verify_v16},
    {16, step_v16_to_v17, verify_v17},
    {17, step_v17_to_v18, verify_v18},
    {0, NULL, NULL}, /* terminator */
};

const struct edc_migration *edc_migrations(void)
{
    return EDC_MIGRATIONS;
}

/* Look a step up in an arbitrary registry (the built-in one for production, a synthetic
 * one in tests). */
static const struct edc_migration *find_in(const struct edc_migration *registry, int from)
{
    if (!registry)
        return NULL;
    for (const struct edc_migration *m = registry; m->from != 0; m++)
        if (m->from == from)
            return m;
    return NULL;
}

const struct edc_migration *edc_migration_find(int from)
{
    return find_in(EDC_MIGRATIONS, from);
}

int edc_migration_lowest_version(void)
{
    return EDC_MIGRATIONS[0].from;
}

/* ══ The runner ════════════════════════════════════════════════════════════════════ */

bool edc_migrate_run(struct sqlite3 *db, const char *db_path, int from, char *err,
                     size_t err_cap)
{
    return edc_migrate_run_with(db, db_path, from, EDC_MIGRATIONS, EDC_DB_VERSION, err,
                                err_cap);
}

bool edc_migrate_run_with(struct sqlite3 *db, const char *db_path, int from,
                          const struct edc_migration *registry, int target, char *err,
                          size_t err_cap)
{
    if (err && err_cap)
        err[0] = '\0';
    if (!db || !db_path)
    {
        set_err(err, err_cap, "no database to migrate");
        return false;
    }
    if (!registry)
    {
        set_err(err, err_cap, "no migration registry");
        return false;
    }
    if (from >= target)
    {
        set_err(err, err_cap,
                "database schema is v%d, but this EdgeClaw (%s) understands v%d -- a newer "
                "EdgeClaw wrote it. Update EdgeClaw rather than downgrading it: an older "
                "binary cannot read a newer schema and writing it back would lose whatever "
                "that version added.",
                from, edc_version(), target);
        return false;
    }
    if (from <= 0)
    {
        set_err(err, err_cap, "cannot migrate from schema v%d (no version stamp)", from);
        return false;
    }

    /* Every step in the chain must exist BEFORE anything is written. Discovering a hole
     * halfway through a multi-step upgrade would leave the database at a version nobody
     * planned for. */
    for (int v = from; v < target; v++)
    {
        if (!find_in(registry, v))
        {
            set_err(err, err_cap,
                    "no migration step from schema v%d: this build upgrades from v%d only. "
                    "Migrate with the python scripts in tools/ first, or use the older "
                    "binary that wrote this database.",
                    v, registry[0].from);
            return false;
        }
    }

    /* Row counts before the first write, to prove afterwards that the migration moved the
     * schema and not the data. */
    struct row_baseline rows[] = {
        {"sessions", 0}, {"groups", 0}, {"tasks", 0}, {"memo_entries", 0}, {"memo_events", 0},
    };
    const size_t n_rows = sizeof rows / sizeof rows[0];
    if (!capture_row_counts(db, rows, n_rows, err, err_cap))
        return false;

    sds backup = NULL;
    char backup_err[512] = {0};
    if (!backup_database(db, db_path, from, &backup, backup_err, sizeof backup_err))
    {
        set_err(err, err_cap, "refusing to migrate: the backup did not succeed (%s). The "
                              "database is untouched.",
                backup_err);
        return false;
    }

    /* One transaction per step, with the version stamp inside it: a crash leaves the file
     * at a complete version, never half-migrated. */
    int v = from;
    for (; v < target; v++)
    {
        const struct edc_migration *m = find_in(registry, v);
        if (!run_sql(db, "BEGIN IMMEDIATE", "begin", err, err_cap))
            goto failed;

        if (!m->step(db, err, err_cap))
        {
            run_sql(db, "ROLLBACK", "rollback", NULL, 0);
            goto failed;
        }

        {
            char stamp[64];
            snprintf(stamp, sizeof stamp, "PRAGMA user_version=%d", v + 1);
            if (!run_sql(db, stamp, "stamp user_version", err, err_cap))
            {
                run_sql(db, "ROLLBACK", "rollback", NULL, 0);
                goto failed;
            }
        }

        if (!run_sql(db, "COMMIT", "commit", err, err_cap))
        {
            run_sql(db, "ROLLBACK", "rollback", NULL, 0);
            goto failed;
        }

        /* The step's own post-condition, outside the transaction: it inspects committed
         * state, which is what the rest of the process will see. */
        if (m->verify && !m->verify(db, err, err_cap))
            goto failed;

        log_info("state_store: migrated schema v%d → v%d", v, v + 1);
    }

    /* Fold the WAL so the upgraded database is one file again. */
    if (!run_sql(db, "PRAGMA wal_checkpoint(TRUNCATE)", "wal checkpoint", err, err_cap))
        goto failed;

    /* The generic post-conditions: the migration moved the schema, not the data, and left
     * a structurally sound file. What the step claims about its own shape is asserted by
     * its verify callback above. */
    {
        char qerr[256] = {0};
        for (size_t i = 0; i < n_rows; i++)
        {
            sds sql = sdscatprintf(sdsempty(), "SELECT count(*) FROM %s", rows[i].table);
            sqlite3_int64 now = 0;
            const bool ok = query_one_int(db, sql, &now, qerr, sizeof qerr);
            sdsfree(sql);
            if (!ok)
            {
                set_err(err, err_cap, "cannot recount %s: %s", rows[i].table, qerr);
                goto failed;
            }
            if (now != rows[i].before)
            {
                set_err(err, err_cap, "%s had %lld rows before migration and %lld after",
                        rows[i].table, (long long)rows[i].before, (long long)now);
                goto failed;
            }
        }
    }

    {
        char check[128] = {0};
        char qerr[256] = {0};
        if (!query_one_text(db, "PRAGMA quick_check(1)", check, sizeof check, qerr, sizeof qerr))
        {
            set_err(err, err_cap, "quick_check failed to run: %s", qerr);
            goto failed;
        }
        if (strcmp(check, "ok") != 0)
        {
            set_err(err, err_cap, "quick_check after migration: %s", check);
            goto failed;
        }
    }

    log_info("state_store: schema migrated v%d → v%d (backup: %s)", from, target,
             backup ? backup : "(none)");
    if (backup)
        sdsfree(backup);
    return true;

failed:
    {
        /* `err` is the buffer the reason was written into, so it cannot also be an argument
         * to the snprintf that overwrites it. Copy it out first. */
        char reason[512];
        snprintf(reason, sizeof reason, "%s",
                 (err && err[0]) ? err : "migration failed");
        set_err(err, err_cap,
                "%s. The database was left at schema v%d; a complete backup is at %s", reason,
                v, backup ? backup : "(none)");
    }
    if (backup)
        sdsfree(backup);
    return false;
}
