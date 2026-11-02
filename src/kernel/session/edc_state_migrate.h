#ifndef EDC_STATE_MIGRATE_H
#define EDC_STATE_MIGRATE_H

/* ── Schema migration ─────────────────────────────────────────────────────────
 *
 * Upgrading a workspace database in place, one version at a time, without ever asking the
 * user to delete a file. The discipline this module implements is not invented here: it is
 * the one the `tools/migrate_edgeclaw_db_v*_to_v*.py` scripts established on real
 * databases (462 MB, 9 sessions, 54k messages), moved into the binary so a user upgrading
 * EdgeClaw gets it by starting EdgeClaw.
 *
 * THE INVARIANTS, in the order they must hold:
 *
 *  1. BACK UP FIRST, AND PROVE THE BACKUP OPENS. A copy that cannot be read is not a
 *     backup, it is a file. Nothing is written to the live database until the copy passes
 *     `PRAGMA quick_check`.
 *  2. ONE TRANSACTION PER STEP, WITH THE VERSION STAMP INSIDE IT. Both the data change and
 *     `PRAGMA user_version` commit together, so a crash leaves the database at a complete
 *     version -- never half-migrated.
 *  3. FAILURES KEEP THE ORIGINAL. A rolling-back step leaves the file exactly as it was,
 *     and the error names the backup. The word "remove" never appears in a migration
 *     diagnostic: the old instruction to delete the database was how a schema bump used to
 *     cost a user every conversation they had.
 *  4. DOWNGRADES ARE REFUSED, NOT ATTEMPTED. A database written by a newer build cannot be
 *     understood by an older one; the only honest answer is to say so and stop.
 *
 * Steps live in a registry (see edc_state_migrate.c) rather than in a switch: the chain is
 * data, so a test can extend it with a synthetic step and exercise every failure branch --
 * backup, atomicity, idempotence, refusal -- before the project ever ships a real bump.
 */

#include <stdbool.h>
#include <stddef.h>

struct sqlite3;

/* One step: reads schema `from`, leaves the database at `from + 1`.
 *
 * The step runs INSIDE a transaction opened by edc_migrate_run(). It must not begin,
 * commit or roll back a transaction of its own, and it must not touch `user_version` --
 * stamping is the runner's job, precisely so a step cannot forget it. It writes a
 * human-readable reason to `err` and returns false to abort; the runner rolls back.
 *
 * `verify` is optional and runs OUTSIDE the transaction, after the step committed: it
 * asserts what the step claims to have achieved (columns that must now exist, invariants
 * that must now hold). Keeping it per-step rather than in the runner is what lets the
 * runner stay generic about row counts while a step still checks its own shape.
 */
struct edc_migration
{
    int from;
    bool (*step)(struct sqlite3 *db, char *err, size_t err_cap);
    bool (*verify)(struct sqlite3 *db, char *err, size_t err_cap);
};

/* The registry, ascending by `from`, terminated by an entry whose `from` is 0. */
const struct edc_migration *edc_migrations(void);

/* The step reading `from`, or NULL when no step exists for it. */
const struct edc_migration *edc_migration_find(int from);

/* The oldest schema version this build can upgrade from: the smallest `from` in the
 * registry. Returns 0 when the registry is empty (nothing can be upgraded yet). */
int edc_migration_lowest_version(void);

/* Bring `db` from `from` up to EDC_DB_VERSION, backing up first.
 *
 * `db_path` is the main database file; the -wal and -shm siblings are derived from it.
 * Returns true only when every step committed, the WAL was folded back, and quick_check
 * passes. On false, `err` holds the reason and -- when the backup succeeded -- its path;
 * the live database is unchanged (rolled back) or, in the worst case, parked at the last
 * completely migrated version, which is still readable by the same binary that wrote it.
 */
bool edc_migrate_run(struct sqlite3 *db, const char *db_path, int from, char *err,
                     size_t err_cap);

/* The runner against an explicit registry and target version.
 *
 * edc_migrate_run is this function with the built-in registry and EDC_DB_VERSION. The
 * parameterised form exists so tests can drive every branch of the RUNNER -- backup,
 * atomicity, rollback, idempotence, refusal, a missing step in the middle of a chain --
 * with a synthetic step, instead of waiting for the project to ship a real schema bump to
 * discover the runner was broken.
 */
bool edc_migrate_run_with(struct sqlite3 *db, const char *db_path, int from,
                          const struct edc_migration *registry, int target, char *err,
                          size_t err_cap);

#endif /* EDC_STATE_MIGRATE_H */
