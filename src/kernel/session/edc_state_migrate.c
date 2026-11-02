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

/* ══ The registry ══════════════════════════════════════════════════════════════════
 * Ascending by `from`, terminated by from == 0. Adding a step is the whole cost of a
 * schema bump now; everything else (backup, transaction, stamp, verify) is the runner's.
 *
 * A step here is only half of a bump: EDC_SCHEMA_SQL in edc_state_store.c describes the
 * CURRENT shape and must change in the same commit, or a fresh workspace and a migrated
 * one disagree. tests/test_state_migrate.c asserts that they agree. */
static const struct edc_migration EDC_MIGRATIONS[] = {
    {7, step_v7_to_v8, verify_v8},
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
