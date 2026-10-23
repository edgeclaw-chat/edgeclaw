/* edc_state_store: the SQLite home of session state.
 *
 * Replaces the `session.json` snapshot writer.
 * This file owns the connection, the schema, the startup safety checks and the
 * PRAGMAs; the read/write mapping lives alongside it.
 *
 * Two things here are easy to get wrong and are called out where they happen:
 *  - the PRAGMA set is a deliberate choice, not a copy of somebody's blog post
 *    (synchronous=NORMAL in particular -- see apply_pragmas);
 *  - the startup checks exist because the workspace is a MODEL-WRITABLE directory.
 */

#include "edc_state_store.h"

#include "edc_config.h"
#include "edc_datetime_utils.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_path.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_state_serializer.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "edc_token_estimate.h"
#include "utlist_fix.h"
#include "cJSON.h"
#include "log.h"
#include "sds.h"
#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/vfs.h>
#endif

#include <uv.h>

/* ── Process-global connection ───────────────────────────────────────────────
 * One workspace per process (design doc §3.1: a single database, not one per session),
 * so a single connection. Deliberately not passed around: the store has no reason to
 * exist twice in one process, and threading it through every session/task call site
 * would be noise. */
static sqlite3 *g_db = NULL;
static sds g_db_path = NULL;   /* resolved absolute path, owned */
static sds g_ws_dir = NULL;    /* resolved workspace it was opened for, owned */
static sds g_ws_raw = NULL;    /* the workspace string as the caller spelled it, owned */
static sds g_lock_path = NULL; /* same file, held via flock; owned */
static int g_lock_fd = -1;

/* Identity of the database file this connection was opened on. If the file is later
 * removed or replaced (a test that wipes its workspace, or someone deleting the database
 * to recreate it), the open handle would keep writing into an unlinked inode and every
 * subsequent read would see nothing. Comparing the identity before each use turns that
 * silent split-brain into a reopen. dev/ino are 0 on platforms without st_ino, where
 * exists_if_same() falls back to "the path still exists". */
static unsigned long long g_db_dev;
static unsigned long long g_db_ino;

/* ── Schema ─────────────────────────────────────────────────────────────────
 * Transcribed from design doc §4. Three rules the DDL follows:
 *   1. every structure in the old session.json has a column (no whole-subtree blobs),
 *      otherwise neither queries nor incremental writes are possible;
 *   2. messages store the message's own JSON verbatim (`json` is the truth) with three
 *      extracted columns for lookup only -- splitting a message into columns would give
 *      the message schema two sources of truth;
 *   3. ordering is explicit (`ord`), never rowid: groups are a linked list and turns are
 *      a utarray, so insertion order is not something SQLite can infer.
 *
 * `end_state` is not called `end`: SQL keyword. `extra` on the two big tables is the
 * escape hatch for fields added later without a DDL change.
 *
 * EVERY timestamp column is INTEGER: seconds since the Unix epoch. That is the one
 * convention the in-memory code already uses (edc_datetime_now() is time(NULL)), so the
 * database does not invent a second one. Two conversions happen at the edges and nowhere
 * else:
 *   - sessions.created_at / last_active are held in memory as the local-time display
 *     string ("%Y-%m-%d %H:%M:%S"); they are parsed with edc_datetime_parse() on write and
 *     formatted back with edc_datetime_format() on read.
 *   - messages.ts is extracted from the message JSON's own "ts" string the same way. The
 *     JSON keeps the string: it is what the transcript shows and what the model sees, and
 *     an integer would lose the local-time rendering.
 * Columns holding 0 mean "unknown" (the same sentinel the C structs use), never 1970. */
static const char *const EDC_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS meta ("
    "  key   TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS sessions ("
    "  session_id            TEXT PRIMARY KEY,"
    "  name                  TEXT NOT NULL,"
    "  created_at            INTEGER,"
    "  last_active           INTEGER,"
    "  workdir               TEXT,"
    "  active_task_id        TEXT,"
    "  foreground_group_id   TEXT,"
    "  next_completion_seq   INTEGER NOT NULL DEFAULT 1,"
    "  archived              INTEGER NOT NULL DEFAULT 0,"
    "  broken                INTEGER NOT NULL DEFAULT 0,"
    "  memo_next_seq         INTEGER NOT NULL DEFAULT 0,"
    "  memo_next_entry_id    INTEGER NOT NULL DEFAULT 0,"
    "  memo_pruned_before    INTEGER NOT NULL DEFAULT 0,"
    "  updated_at            INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS groups ("
    "  session_id         TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,"
    "  group_id           TEXT NOT NULL,"
    "  leader_task_id     TEXT,"
    "  is_orphaned        INTEGER NOT NULL DEFAULT 0,"
    "  kind               TEXT NOT NULL DEFAULT 'user',"
    "  memory             TEXT,"
    "  fold_epoch         INTEGER NOT NULL DEFAULT 0,"
    "  memory_created_at  INTEGER NOT NULL DEFAULT 0,"
    "  memory_updated_at  INTEGER NOT NULL DEFAULT 0,"
    "  memory_covers_to   INTEGER NOT NULL DEFAULT 0,"
    "  ord                INTEGER NOT NULL DEFAULT 0,"
    "  extra              TEXT,"
    "  PRIMARY KEY (session_id, group_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS groups_by_session ON groups(session_id, ord);"

    "CREATE TABLE IF NOT EXISTS group_fold_files ("
    "  session_id TEXT NOT NULL,"
    "  group_id   TEXT NOT NULL,"
    "  ord        INTEGER NOT NULL,"
    "  path       TEXT NOT NULL,"
    "  PRIMARY KEY (session_id, group_id, ord),"
    "  FOREIGN KEY (session_id, group_id) REFERENCES groups(session_id, group_id) ON DELETE CASCADE"
    ");"

    "CREATE TABLE IF NOT EXISTS tasks ("
    "  task_id                   TEXT PRIMARY KEY,"
    "  session_id                TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,"
    "  group_id                  TEXT NOT NULL,"
    "  ord                       INTEGER NOT NULL DEFAULT 0,"
    "  description               TEXT,"
    "  raw_input                 TEXT,"
    "  step                      TEXT,"
    "  life                      TEXT,"
    "  end_state                 TEXT,"
    "  blocked_by                TEXT,"
    "  blocked_end               TEXT,"
    "  fail                      TEXT,"
    "  is_group_leader           INTEGER NOT NULL DEFAULT 0,"
    "  bg                        INTEGER NOT NULL DEFAULT 0,"
    "  tool_only                 INTEGER NOT NULL DEFAULT 0,"
    "  deferred_vars             INTEGER NOT NULL DEFAULT 0,"
    "  memo_kind                 TEXT NOT NULL DEFAULT 'curate',"
    "  sink                      TEXT,"
    "  sink_group_id             TEXT,"
    "  fold_cut_task             TEXT,"
    "  fold_cut_turn             INTEGER NOT NULL DEFAULT 0,"
    "  fold_cut_prefix           INTEGER NOT NULL DEFAULT 0,"
    "  memo_cut_task             TEXT,"
    "  memo_cut_turn             INTEGER NOT NULL DEFAULT 0,"
    "  memo_cut_prefix           INTEGER NOT NULL DEFAULT 0,"
    "  forked_from_task_id       TEXT REFERENCES tasks(task_id),"
    "  fork_point_completion_seq INTEGER NOT NULL DEFAULT 0,"
    "  output                    TEXT,"
    "  completion_seq            INTEGER NOT NULL DEFAULT 0,"
    "  tokens_prompt             INTEGER NOT NULL DEFAULT 0,"
    "  tokens_completion         INTEGER NOT NULL DEFAULT 0,"
    "  tokens_total              INTEGER NOT NULL DEFAULT 0,"
    "  tokens_cached             INTEGER NOT NULL DEFAULT 0,"
    "  last_total_tokens         INTEGER NOT NULL DEFAULT 0,"
    "  last_fold_epoch           INTEGER NOT NULL DEFAULT 0,"
    "  last_hist_len             INTEGER NOT NULL DEFAULT 0,"
    "  last_turn_len             INTEGER NOT NULL DEFAULT 0,"
    "  memo_seen_seq             INTEGER NOT NULL DEFAULT 0,"
    "  last_prefix_tokens        INTEGER NOT NULL DEFAULT 0,"
    "  redirect_stdout           TEXT,"
    "  redirect_stderr           TEXT,"
    "  redirect_append           INTEGER NOT NULL DEFAULT 0,"
    "  redirect_stderr_append    INTEGER NOT NULL DEFAULT 0,"
    "  todos                     TEXT,"
    "  todo_sync_gap             INTEGER NOT NULL DEFAULT 0,"
    "  extra                     TEXT,"
    "  updated_at                INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS tasks_by_group ON tasks(session_id, group_id, ord);"
    "CREATE INDEX IF NOT EXISTS tasks_by_fork  ON tasks(forked_from_task_id);"

    "CREATE TABLE IF NOT EXISTS task_deps ("
    "  target_task_id TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,"
    "  source_task_id TEXT NOT NULL,"
    "  type           TEXT NOT NULL,"
    "  completed      INTEGER NOT NULL DEFAULT 0,"
    "  cancelled      INTEGER NOT NULL DEFAULT 0,"
    "  injected       INTEGER NOT NULL DEFAULT 0,"
    "  ord            INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (target_task_id, source_task_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS task_deps_by_source ON task_deps(source_task_id);"

    "CREATE TABLE IF NOT EXISTS task_tool_calls ("
    "  task_id TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,"
    "  ord     INTEGER NOT NULL,"
    "  call_id TEXT,"
    "  name    TEXT,"
    "  args    TEXT,"
    "  status  TEXT NOT NULL,"
    "  PRIMARY KEY (task_id, ord)"
    ");"

    "CREATE TABLE IF NOT EXISTS turns ("
    "  task_id       TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,"
    "  turn_ord      INTEGER NOT NULL,"
    "  folded_prefix INTEGER NOT NULL DEFAULT 0,"
    "  memo_prefix   INTEGER NOT NULL DEFAULT 0,"
    "  is_active     INTEGER NOT NULL DEFAULT 0,"
    /* Materialized so the runtime never has to walk the rows to answer "how long" or
     * "how many tokens": the same role struct edc_turn used to play in memory. */
    "  msg_count     INTEGER NOT NULL DEFAULT 0,"
    "  token_count   INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (task_id, turn_ord)"
    ");"

    "CREATE TABLE IF NOT EXISTS messages ("
    "  id       TEXT NOT NULL PRIMARY KEY,"
    "  task_id  TEXT NOT NULL,"
    "  turn_ord INTEGER NOT NULL,"
    "  msg_ord  INTEGER NOT NULL,"
    "  role     TEXT,"
    "  mid      TEXT,"
    "  ts       INTEGER,"
    "  json     TEXT NOT NULL,"
    /* Token estimate, computed once when the message is appended. Token accounting is
     * then a SUM over an index range instead of re-estimating every message on every
     * request. */
    "  tokens   INTEGER NOT NULL DEFAULT 0,"
    "  FOREIGN KEY (task_id, turn_ord) REFERENCES turns(task_id, turn_ord) ON DELETE CASCADE"
    ");"
    /* The positional key still needs its own index: with `id` as the primary key, the
     * loader's range scan (WHERE task_id=? AND turn_ord=? ORDER BY msg_ord) has nothing
     * to use otherwise. MEASURED: without this index that query is a full scan plus a
     * temp B-tree -- 13683 ms instead of 33.9 ms for a 40108-message session, a 400x
     * regression that is invisible in any small test. With it, the cost of having `id` as
     * the primary key instead of the positional key is +23% on the flush (0.0252 ->
     * 0.0310 ms) and nothing on the load. */
    "CREATE UNIQUE INDEX IF NOT EXISTS messages_natural ON messages(task_id, turn_ord, msg_ord);"
    "CREATE INDEX IF NOT EXISTS messages_by_mid ON messages(mid) WHERE mid IS NOT NULL;"
    /* DELIBERATELY a rowid table (no WITHOUT ROWID) -- measured, not assumed.
     *
     * The obvious design is WITHOUT ROWID: the PK is (task_id, turn_ord, msg_ord) and the
     * loader reads a turn's messages as a PK range scan, which looks like the one case
     * WITHOUT ROWID is perfect for. It measured 44% SLOWER than a rowid table:
     *
     *     reading every message of a 40108-message session (median of 5)
     *       WITHOUT ROWID                 58.88 ms
     *       rowid table + PK index        32.92 ms      (-44%)
     *     steady-state flush, 3 messages
     *       WITHOUT ROWID                  0.039 ms
     *       rowid table + PK index         0.025 ms      (-36%)
     *
     * Why: WITHOUT ROWID puts the row -- including the ~4 KB `json` -- in the PK B-tree's
     * leaf pages, so the leaves are huge and the index itself is the data. A rowid table
     * keeps the PK index small (keys + rowid) and puts the payload in a table clustered by
     * insertion order, which matches how the loader walks tasks. opencode's schema does
     * the same thing for message rows.
     *
     * This also deletes a hazard recorded earlier: on a WITHOUT ROWID table, adding a
     * secondary index on a PK prefix (task_id) made the planner take that index and then
     * hop into the PK B-tree for the payload -- 5x slower loads (54 -> 280 ms). On a
     * rowid table the same index costs ~10% and the planner keeps using the PK index, so
     * the trap is gone. There is nevertheless no index on messages(task_id): the PK index
     * already answers `count(*)` as a COVERING INDEX, without touching `json`.
     *
     * `PRIMARY KEY` on a rowid table creates a real unique index, so ON CONFLICT
     * (task_id, turn_ord, msg_ord) DO UPDATE ... WHERE works -- the stage-2 upsert.
     */


    "CREATE TABLE IF NOT EXISTS memo_entries ("
    "  session_id  TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,"
    "  entry_id    TEXT NOT NULL,"
    "  title       TEXT,"
    "  summary     TEXT,"
    "  detail      TEXT,"
    "  sources     TEXT,"
    "  proposed_by TEXT,"
    "  created_seq INTEGER NOT NULL DEFAULT 0,"
    "  updated_seq INTEGER NOT NULL DEFAULT 0,"
    "  created_at  INTEGER NOT NULL DEFAULT 0,"
    "  updated_at  INTEGER NOT NULL DEFAULT 0,"
    "  superseded  INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (session_id, entry_id)"
    ");"

    "CREATE TABLE IF NOT EXISTS memo_events ("
    "  session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,"
    "  seq        INTEGER NOT NULL,"
    "  ts         INTEGER NOT NULL DEFAULT 0,"
    "  entry_id   TEXT,"
    "  kind       TEXT,"
    "  title      TEXT,"
    "  summary    TEXT,"
    "  PRIMARY KEY (session_id, seq)"
    ");";

/* The tables a healthy database must contain. Used by the post-DDL sanity check so a
 * half-applied schema fails here rather than as a mysterious SQL error later. */
static const char *const EDC_EXPECTED_TABLES[] = {
    "meta", "sessions", "groups", "group_fold_files", "tasks", "task_deps",
    "task_tool_calls", "turns", "messages", "memo_entries", "memo_events", NULL};

/* ── Small SQL helpers ─────────────────────────────────────────────────────── */

/* Run a statement, logging the SQLite error on failure. */
static bool exec_sql(sqlite3 *db, const char *sql, const char *what)
{
    char *err = NULL;
    const int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK)
    {
        log_error("state_store: %s failed: rc=%d %s", what, rc, err ? err : sqlite3_errmsg(db));
        if (err)
            sqlite3_free(err);
        return false;
    }
    if (err)
        sqlite3_free(err);
    return true;
}

/* Evaluate a PRAGMA that yields one text row. Returns a malloc'd copy the caller owns,
 * or NULL. Used to VERIFY a pragma rather than assume sqlite applied it. */
static char *query_text(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t)
            out = strdup((const char *)t);
    }
    sqlite3_finalize(st);
    return out;
}

static int query_int(sqlite3 *db, const char *sql, int fallback)
{
    sqlite3_stmt *st = NULL;
    int out = fallback;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return fallback;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return out;
}

/* ── Startup safety: filesystem type ────────────────────────────────────────
 * SQLite's locking is documented to be unreliable on network filesystems; a shared
 * workspace over NFS/SMB would corrupt the database. This is a whitelist of local
 * filesystems, so an unknown/remote one refuses startup (design doc §7.7). There is no
 * JSON fallback to degrade to -- refusing is the only safe answer. */
#if defined(__linux__)
static bool fs_type_is_local(const char *dir, const char **why)
{
    struct statfs sfs;
    if (statfs(dir, &sfs) != 0)
    {
        *why = "cannot stat the workspace filesystem";
        return false;
    }
    switch ((unsigned long)sfs.f_type)
    {
    case 0xEF53:       /* ext2/3/4 */
    case 0x58465342:   /* XFS */
    case 0x9123683E:   /* BTRFS */
    case 0x01021994:   /* tmpfs */
    case 0x2FC12FC1:   /* ZFS */
    case 0xF2F52010:   /* F2FS */
    case 0x794C7630:   /* overlayfs (containers) */
    case 0xCA451A4E:   /* bcachefs */
        return true;
    default:
        *why = "not a local filesystem (SQLite locking is unreliable over network mounts)";
        return false;
    }
}
#else
static bool fs_type_is_local(const char *dir, const char **why)
{
    (void)dir;
    (void)why;
    /* Windows (NTFS/ReFS) and macOS (APFS/HFS+) are local by construction here; flock-
     * backed locking is replaced by SQLite's own Windows locking. Nothing to check. */
    return true;
}
#endif

/* ── Startup safety: symlinks ───────────────────────────────────────────────
 * The workspace is writable by the model, which has already planted
 * `workspace/jobs.json -> ~/.edgeclaw/config.json` once (see edc_file_fopen_safe). A
 * symlinked database would let a model redirect our writes onto any file the process can
 * open, so any of the three files being a link refuses startup. */
static bool refuse_symlink(const char *path, const char *what)
{
#ifndef _WIN32
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode))
    {
        log_error("state_store: refusing to start: %s (%s) is a symbolic link", path, what);
        return false;
    }
#else
    (void)path;
    (void)what;
#endif
    return true;
}

/* Realpath of an existing directory, or NULL. */
static sds resolve_dir(const char *path)
{
    uv_fs_t req;
    sds out = NULL;
    if (uv_fs_realpath(uv_default_loop(), &req, path, NULL) == 0 && req.ptr)
        out = sdsnew((const char *)req.ptr);
    uv_fs_req_cleanup(&req);
    return out;
}

/* ── Locking ────────────────────────────────────────────────────────────────
 * The database allows concurrent processes, but two agent processes holding two
 * divergent in-memory trees would overwrite each other's semantics. One process per
 * workspace, enforced with a non-blocking exclusive flock on the database file itself
 * (a separate .lock file would be one more thing to symlink).
 *
 * The descriptor stays open for the process lifetime: closing it releases the lock. */
static bool acquire_lock(const char *path)
{
#ifdef _WIN32
    /* Windows: SQLite's own locking plus a sharing-mode open is the equivalent; the
     * exclusive-open contract is enforced by the OS on the file handle below. */
    g_lock_fd = _open(path, _O_RDWR | _O_CREAT | _O_BINARY, 0644);
    if (g_lock_fd < 0)
    {
        log_error("state_store: cannot open %s for locking: %s", path, strerror(errno));
        return false;
    }
    return true;
#else
    g_lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (g_lock_fd < 0)
    {
        log_error("state_store: cannot open %s for locking: %s", path, strerror(errno));
        return false;
    }
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0)
    {
        if (errno == EWOULDBLOCK)
            log_error("state_store: another edgeclaw instance is using this workspace "
                      "(%s is locked). Refusing to start -- two processes would overwrite "
                      "each other's session state.",
                      path);
        else
            log_error("state_store: flock(%s) failed: %s", path, strerror(errno));
        close(g_lock_fd);
        g_lock_fd = -1;
        return false;
    }
    return true;
#endif
}

static void release_lock(void)
{
    if (g_lock_fd >= 0)
    {
#ifndef _WIN32
        flock(g_lock_fd, LOCK_UN);
#endif
#ifdef _WIN32
        _close(g_lock_fd);
#else
        close(g_lock_fd);
#endif
        g_lock_fd = -1;
    }
}

/* ── PRAGMAs ────────────────────────────────────────────────────────────────
 * Why each one, because this is the set that decides durability and safety:
 *
 *   journal_mode=WAL          readers and the writer stop blocking each other, and a
 *                             crash recovers by replaying -wal. Verified below, since
 *                             SQLite silently keeps the old mode if the file cannot
 *                             support it.
 *
 *   synchronous=NORMAL        DELIBERATE, and the one setting worth arguing about.
 *                             In WAL mode this means COMMIT does not fsync (sqlite3.c:
 *                             "with synchronous=NORMAL in WAL mode, transaction commit
 *                             is not synced"); all fsyncs move to the checkpoint.
 *                             Measured on this project's data (ext4, LVM, 50 KB/tx):
 *                             NORMAL 0.047 ms vs FULL 6.57 ms per commit -- and 6.57 ms
 *                             alone would break the "P95 < 5 ms" acceptance target.
 *                             What NORMAL keeps: a `kill -9` loses nothing (the data is
 *                             already in the page cache), and a power cut loses recent
 *                             transactions but leaves the DATABASE CONSISTENT.
 *                             What it gives up versus FULL: the last transaction may be
 *                             lost on power loss.
 *                             Compared with the code this replaces -- write + rename with
 *                             NO fsync anywhere in the repo -- that is not a downgrade:
 *                             the old path could leave a TRUNCATED session.json (which
 *                             the loader then quarantined as .broken, i.e. a whole
 *                             session lost), whereas WAL rolls back to the last valid
 *                             commit.
 *
 *   busy_timeout=5000         the scheduler thread must not fail on a transient lock.
 *   foreign_keys=ON           the ON DELETE CASCADE cleanup depends on it, and it is OFF
 *                             by default per connection.
 *   trusted_schema=OFF        the workspace is model-writable: if a model plants a view
 *                             or trigger in this file, schema parsing must not execute
 *                             dangerous expressions.
 *   wal_autocheckpoint=1000   ~4 MB of WAL before an automatic checkpoint; a deep idle
 *                             checkpoint (TRUNCATE) happens on close.
 */
static bool apply_pragmas(sqlite3 *db)
{
    if (!exec_sql(db, "PRAGMA journal_mode=WAL", "PRAGMA journal_mode"))
        return false;

    /* The page cache is the session cache. This is the answer to "if writing to disk is
     * slow, make the cache bigger instead of building a second copy of the database in
     * C": the message rows the runtime re-reads (the live tail of a conversation) stay
     * resident as PAGES, and every reader -- fold, token accounting, request assembly --
     * sees them through the same cache. 64 MiB is ~16k pages, comfortably more than the
     * live tail of a long session (a 40k-message session is ~45k pages in total, and the
     * unfolded tail is a fraction of that).
     *
     * Negative = KiB and per-connection, so this never rewrites a persisted setting into
     * somebody's database file. */
    if (!exec_sql(db, "PRAGMA cache_size=-65536", "PRAGMA cache_size"))
        return false;

    /* Memory-map the read path. Read-only mapping: writes still go through the WAL, so
     * this cannot bypass the durability story above. 512 MiB ceiling; SQLite clamps it to
     * the file size, so a small database maps only what exists. */
    if (!exec_sql(db, "PRAGMA mmap_size=536870912", "PRAGMA mmap_size"))
        return false;

    /* Verify instead of trusting: on a filesystem that cannot do WAL, SQLite keeps the
     * previous journal mode and only reports it here. */
    char *mode = query_text(db, "PRAGMA journal_mode");
    const bool is_wal = mode && strcmp(mode, "wal") == 0;
    if (!is_wal)
        log_error("state_store: journal_mode is '%s', expected 'wal' -- the filesystem may "
                  "not support it",
                  mode ? mode : "(none)");
    free(mode);
    if (!is_wal)
        return false;

    return exec_sql(db, "PRAGMA synchronous=NORMAL", "PRAGMA synchronous") &&
           exec_sql(db, "PRAGMA busy_timeout=5000", "PRAGMA busy_timeout") &&
           exec_sql(db, "PRAGMA foreign_keys=ON", "PRAGMA foreign_keys") &&
           exec_sql(db, "PRAGMA trusted_schema=OFF", "PRAGMA trusted_schema") &&
           exec_sql(db, "PRAGMA wal_autocheckpoint=1000", "PRAGMA wal_autocheckpoint");
}

/* ── Version handling ───────────────────────────────────────────────────────
 * Exactly one version, no migration code (design doc §3.3). A mismatch is not a data
 * incident in a development build; it means "this build changed the schema, recreate the
 * file", and the message says exactly what to delete. */
static bool check_or_create_schema(sqlite3 *db)
{
    const int version = query_int(db, "PRAGMA user_version", -1);
    if (version < 0)
    {
        log_error("state_store: cannot read PRAGMA user_version");
        return false;
    }

    if (version == EDC_DB_VERSION)
        return true;

    if (version != 0)
    {
        log_error("state_store: database schema is v%d, this build expects v%d. This is a "
                  "development build with no migration path -- remove %s (and its -wal / "
                  "-shm) to recreate it.",
                  version, EDC_DB_VERSION, g_db_path ? g_db_path : "(the database)");
        return false;
    }

    /* user_version == 0: a brand-new file (or one created outside this code). Build the
     * schema and stamp the version. */
    if (!exec_sql(db, EDC_SCHEMA_SQL, "schema creation"))
        return false;
    {
        /* Stamped from the macro, not a literal: a hardcoded number here would silently
         * disagree with EDC_DB_VERSION the next time the schema changes (it did once). */
        char stamp[64];
        snprintf(stamp, sizeof(stamp), "PRAGMA user_version=%d", EDC_DB_VERSION);
        if (!exec_sql(db, stamp, "PRAGMA user_version"))
            return false;
    }

    /* Sanity: every expected table must exist now. A partial schema should fail here
     * rather than as a confusing SQL error in the middle of a flush. */
    for (int i = 0; EDC_EXPECTED_TABLES[i]; i++)
    {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s'",
                 EDC_EXPECTED_TABLES[i]);
        if (query_int(db, sql, 0) != 1)
        {
            log_error("state_store: schema creation left no '%s' table", EDC_EXPECTED_TABLES[i]);
            return false;
        }
    }

    log_info("state_store: created schema v%d at %s", EDC_DB_VERSION, g_db_path);
    return true;
}

/* ── Open / close ─────────────────────────────────────────────────────────── */

/* Is the file this connection was opened on still the file at g_db_path? */
static bool db_file_is_still_ours(void)
{
    if (!g_db_path)
        return false;
    struct stat st;
    if (stat(g_db_path, &st) != 0)
        return false; /* deleted underneath us */
#ifndef _WIN32
    if (g_db_ino != 0 || g_db_dev != 0)
        return ((unsigned long long)st.st_ino == g_db_ino) &&
               ((unsigned long long)st.st_dev == g_db_dev);
#endif
    return true;
}

/* Resolve the workspace a config names, or NULL. */
static sds workspace_of(struct edc_config *cfg)
{
    if (!cfg || !cfg->workspace || sdslen(cfg->workspace) == 0)
        return NULL;
    return resolve_dir(cfg->workspace);
}

bool edc_state_store_open(struct edc_config *cfg)
{
    /* Already open: reuse it, unless the caller is asking about a different workspace or
     * the file we hold has been replaced. Both are "start over" situations, and the
     * handle is cheap to rebuild -- the expensive thing is the failed write that would
     * have gone into an unlinked inode. */
    if (g_db)
    {
        /* Reuse only when it is demonstrably still the same thing: the caller named the
         * same workspace AND the file we hold is still the file at that path. Anything
         * less -- a different workspace, or a workspace that was deleted or replaced
         * underneath us -- must reopen, because writing into an unlinked inode would
         * silently accumulate state nothing can ever read. */
        bool reuse = false;
        if (!cfg || !cfg->workspace || sdslen(cfg->workspace) == 0)
        {
            reuse = true; /* no workspace named: keep what we have */
        }
        else if (g_ws_raw && strcmp(cfg->workspace, g_ws_raw) == 0)
        {
            /* Cheap path: same string, so only the file identity needs checking. */
            reuse = db_file_is_still_ours();
        }
        else
        {
            sds want = workspace_of(cfg);
            reuse = want && g_ws_dir && strcmp(want, g_ws_dir) == 0 && db_file_is_still_ours();
            if (want)
                sdsfree(want);
        }
        if (reuse)
            return true;
        log_info("state_store: reopening (workspace changed, or the database file was "
                 "removed or replaced)");
        edc_state_store_close();
    }

    if (!cfg || !cfg->workspace || sdslen(cfg->workspace) == 0)
    {
        log_error("state_store: no workspace configured");
        return false;
    }

    /* Resolve the workspace FIRST and derive every database path from the resolved
     * form: that is what stops a symlinked workspace from becoming an escape hatch, and
     * it keeps the -wal/-shm checks looking at the real directory. */
    /* Create the workspace BEFORE resolving it: realpath() needs the directory to exist,
     * and a first run (or a test that wiped its workspace) legitimately has no directory
     * yet. Failing here would make every save into a fresh workspace impossible. */
    {
        EdcError *mk_err = NULL;
        if (!edc_dir_create_with_parents(cfg->workspace, 0755, &mk_err))
        {
            log_error("state_store: cannot create workspace %s: %s", cfg->workspace,
                      mk_err ? mk_err->message : "?");
            if (mk_err)
                edc_error_free(mk_err);
            return false;
        }
        if (mk_err)
            edc_error_free(mk_err);
    }

    sds ws = resolve_dir(cfg->workspace);
    if (!ws)
    {
        log_error("state_store: workspace '%s' is not a directory I can resolve", cfg->workspace);
        return false;
    }

    const char *fs_why = NULL;
    if (!fs_type_is_local(ws, &fs_why))
    {
        log_error("state_store: refusing to start: workspace %s: %s", ws, fs_why);
        sdsfree(ws);
        return false;
    }

    g_db_path = edc_path_join(ws, "edgeclaw.db", NULL);
    if (g_ws_dir)
        sdsfree(g_ws_dir);
    g_ws_dir = sdsdup(ws);
    if (g_ws_raw)
        sdsfree(g_ws_raw);
    g_ws_raw = sdsdup(cfg->workspace);
    sdsfree(ws);

    /* Refuse the three files being symlinks BEFORE opening anything. -wal/-shm may not
     * exist yet, which refuse_symlink treats as fine. */
    if (!refuse_symlink(g_db_path, "database"))
        goto fail;
    {
        sds wal = sdscatprintf(sdsempty(), "%s-wal", g_db_path);
        sds shm = sdscatprintf(sdsempty(), "%s-shm", g_db_path);
        const bool ok = refuse_symlink(wal, "write-ahead log") && refuse_symlink(shm, "shared memory");
        sdsfree(wal);
        sdsfree(shm);
        if (!ok)
            goto fail;
    }

    /* One process per workspace. */
    g_lock_path = sdsdup(g_db_path);
    if (!acquire_lock(g_lock_path))
        goto fail;

    /* Open the connection. Serialized threading (the library default) is fine: one writer
     * thread, and SQLite serializes for us if a reader ever appears. */
    const int rc = sqlite3_open_v2(g_db_path, &g_db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK)
    {
        log_error("state_store: cannot open %s: rc=%d %s", g_db_path, rc,
                  g_db ? sqlite3_errmsg(g_db) : "out of memory");
        goto fail;
    }

    /* Cheap corruption probe before anything else touches the file. On failure the WAL
     * is left in place for `edgeclaw db salvage` rather than silently discarded. */
    {
        char *integrity = query_text(g_db, "PRAGMA quick_check(1)");
        const bool ok = integrity && strcmp(integrity, "ok") == 0;
        if (!ok)
            log_error("state_store: database %s failed quick_check: %s (keep the -wal file "
                      "for salvage)",
                      g_db_path, integrity ? integrity : "(no answer)");
        free(integrity);
        if (!ok)
            goto fail;
    }

    if (!apply_pragmas(g_db))
        goto fail;

    if (!check_or_create_schema(g_db))
        goto fail;

    /* Remember which file this connection is writing to, so a later open() can tell
     * whether the file was replaced underneath it (see db_file_is_still_ours). */
    {
        struct stat st;
        g_db_dev = 0;
        g_db_ino = 0;
        if (stat(g_db_path, &st) == 0)
        {
#ifndef _WIN32
            g_db_dev = (unsigned long long)st.st_dev;
            g_db_ino = (unsigned long long)st.st_ino;
#endif
        }
    }

    log_info("state_store: opened %s (schema v%d)", g_db_path, EDC_DB_VERSION);
    return true;

fail:
    if (g_db)
    {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    release_lock();
    sdsfree(g_lock_path);
    g_lock_path = NULL;
    sdsfree(g_db_path);
    g_db_path = NULL;
    sdsfree(g_ws_dir);
    g_ws_dir = NULL;
    sdsfree(g_ws_raw);
    g_ws_raw = NULL;
    g_db_dev = 0;
    g_db_ino = 0;
    return false;
}

void edc_state_store_close(void)
{
    if (g_db)
    {
        /* Fold the WAL back into the database so a stopped process leaves one file (plus
         * whatever a concurrent reader still needs). Cheap and it keeps `edgeclaw db`
         * inspection honest. Failure is a warning, not an error: the data is committed
         * either way. */
        char *err = NULL;
        const int rc = sqlite3_exec(g_db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, &err);
        if (rc != SQLITE_OK)
            log_warn("state_store: checkpoint on close failed: %s", err ? err : sqlite3_errmsg(g_db));
        if (err)
            sqlite3_free(err);

        /* PRAGMA optimize runs the analysis the query planner would otherwise skip. */
        sqlite3_exec(g_db, "PRAGMA optimize", NULL, NULL, NULL);

        sqlite3_close(g_db);
        g_db = NULL;
    }

    release_lock();
    sdsfree(g_lock_path);
    g_lock_path = NULL;
    sdsfree(g_db_path);
    g_db_path = NULL;
    sdsfree(g_ws_dir);
    g_ws_dir = NULL;
    sdsfree(g_ws_raw);
    g_ws_raw = NULL;
    g_db_dev = 0;
    g_db_ino = 0;
}

bool edc_state_store_is_open(void)
{
    return g_db != NULL;
}

const char *edc_state_store_path(void)
{
    return g_db_path;
}

/* A PRAGMA name is interpolated into SQL, so it has to be a bare identifier. Without this
 * the accessor would be a SQL injection point for anyone who can reach it. */
static bool pragma_name_is_bare(const char *name)
{
    if (!name || !*name)
        return false;
    for (const char *p = name; *p; p++)
    {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok)
            return false;
    }
    return true;
}

int edc_state_store_pragma_int(const char *pragma_name)
{
    if (!g_db || !pragma_name_is_bare(pragma_name))
        return -1;
    char sql[128];
    snprintf(sql, sizeof(sql), "PRAGMA %s", pragma_name);
    return query_int(g_db, sql, -1);
}

char *edc_state_store_pragma_text(const char *pragma_name)
{
    if (!g_db || !pragma_name_is_bare(pragma_name))
        return NULL;
    char sql[128];
    snprintf(sql, sizeof(sql), "PRAGMA %s", pragma_name);
    return query_text(g_db, sql);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Reading the serialized tree
 *
 *  flush() takes the tree edc_state_serialize() already produces and writes it into
 *  columns; load() builds that same tree back from the columns. Both directions share
 *  one vocabulary (the tree's key names), which is why neither has to know how an enum
 *  is spelled -- edc_task_life_name() and friends stay the only place that decides.
 *
 *  These helpers are all "read defensively": the tree is ours, but a row can hold a
 *  NULL where the serializer would have written a value, and a load must not crash on
 *  data a previous version wrote.
 * ══════════════════════════════════════════════════════════════════════════ */

static const char *js_str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem((cJSON *)o, key);
    return (v && cJSON_IsString(v)) ? cJSON_GetStringValue((cJSON *)v) : NULL;
}

static double js_num(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem((cJSON *)o, key);
    return (v && cJSON_IsNumber(v)) ? cJSON_GetNumberValue((cJSON *)v) : 0;
}

static int js_bool(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem((cJSON *)o, key);
    return (v && cJSON_IsTrue(v)) ? 1 : 0;
}

static void bind_str(sqlite3_stmt *st, int i, const char *s)
{
    if (s && *s)
        sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, i);
}

static void bind_num(sqlite3_stmt *st, int i, double d)
{
    sqlite3_bind_int64(st, i, (sqlite3_int64)d);
}

/* A timestamp column from a display string ("%Y-%m-%d %H:%M:%S"). 0 = unknown, the
 * sentinel the C structs already use. */
static sqlite3_int64 ts_from_display(const char *s)
{
    time_t t = 0;
    if (!s || !*s || !edc_datetime_parse(s, &t))
        return 0;
    return (sqlite3_int64)t;
}

static sds ts_to_display(sqlite3_int64 t)
{
    if (t <= 0)
        return sdsempty();
    return edc_datetime_format((time_t)t);
}

/* Prepare, run to completion, finalize. On error the SQLite message is logged with the
 * statement's name so a failure names the table it happened in. */
static bool run_stmt(sqlite3 *db, const char *sql, const char *what,
                     void (*binder)(sqlite3_stmt *, void *), void *ctx)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
    {
        log_error("state_store: prepare %s failed: %s", what, sqlite3_errmsg(db));
        return false;
    }
    if (binder)
        binder(st, ctx);
    const int rc = sqlite3_step(st);
    const bool ok = (rc == SQLITE_DONE || rc == SQLITE_ROW);
    if (!ok)
        log_error("state_store: %s failed: rc=%d %s", what, rc, sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  flush: tree -> columns
 * ══════════════════════════════════════════════════════════════════════════ */

/* NOTE ON "INSERT OR REPLACE": never use it on an ancestor of `messages`.
 * REPLACE deletes the conflicting row first, and ON DELETE CASCADE would take the whole
 * conversation with it. These are true upserts (ON CONFLICT ... DO UPDATE), which leave
 * the row -- and therefore its children -- in place. */
static const char *INSERT_SESSION =
    "INSERT INTO sessions(session_id,name,created_at,last_active,workdir,active_task_id,"
    "foreground_group_id,next_completion_seq,updated_at) VALUES(?,?,?,?,?,?,?,?,?) "
    "ON CONFLICT(session_id) DO UPDATE SET name=excluded.name,created_at=excluded.created_at,"
    "last_active=excluded.last_active,workdir=excluded.workdir,active_task_id=excluded.active_task_id,"
    "foreground_group_id=excluded.foreground_group_id,next_completion_seq=excluded.next_completion_seq,"
    "updated_at=excluded.updated_at";

struct session_bind
{
    const cJSON *sj;
    const char *session_id;
};

static void bind_session(sqlite3_stmt *st, void *ctx)
{
    struct session_bind *b = ctx;
    const cJSON *sj = b->sj;
    bind_str(st, 1, b->session_id);
    bind_str(st, 2, js_str(sj, "name") ? js_str(sj, "name") : "");
    sqlite3_bind_int64(st, 3, ts_from_display(js_str(sj, "created_at")));
    sqlite3_bind_int64(st, 4, ts_from_display(js_str(sj, "last_active")));
    bind_str(st, 5, js_str(sj, "workdir"));
    bind_str(st, 6, js_str(sj, "active_task_id"));
    bind_str(st, 7, js_str(sj, "foreground_group_id"));
    bind_num(st, 8, js_num(sj, "next_completion_seq"));
    sqlite3_bind_int64(st, 9, (sqlite3_int64)time(NULL));
}

static const char *INSERT_GROUP =
    "INSERT INTO groups(session_id,group_id,leader_task_id,is_orphaned,kind,memory,fold_epoch,"
    "memory_created_at,memory_updated_at,memory_covers_to,ord) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
    "ON CONFLICT(session_id,group_id) DO UPDATE SET leader_task_id=excluded.leader_task_id,"
    "is_orphaned=excluded.is_orphaned,kind=excluded.kind,memory=excluded.memory,"
    "fold_epoch=excluded.fold_epoch,memory_created_at=excluded.memory_created_at,"
    "memory_updated_at=excluded.memory_updated_at,memory_covers_to=excluded.memory_covers_to,"
    "ord=excluded.ord";

struct group_bind
{
    const cJSON *gj;
    const char *session_id;
    int ord;
};

static void bind_group(sqlite3_stmt *st, void *ctx)
{
    struct group_bind *b = ctx;
    const cJSON *g = b->gj;
    bind_str(st, 1, b->session_id);
    bind_str(st, 2, js_str(g, "group_id"));
    bind_str(st, 3, js_str(g, "leader_task_id"));
    sqlite3_bind_int(st, 4, js_bool(g, "is_orphaned"));
    bind_str(st, 5, js_str(g, "kind") ? js_str(g, "kind") : "user");
    bind_str(st, 6, js_str(g, "memory"));
    bind_num(st, 7, js_num(g, "fold_epoch"));
    bind_num(st, 8, js_num(g, "memory_created_at"));
    bind_num(st, 9, js_num(g, "memory_updated_at"));
    bind_num(st, 10, js_num(g, "memory_covers_to"));
    sqlite3_bind_int(st, 11, b->ord);
}

static const char *INSERT_TASK =
    "INSERT INTO tasks(task_id,session_id,group_id,ord,description,raw_input,step,life,end_state,"
    "blocked_by,blocked_end,fail,is_group_leader,bg,tool_only,deferred_vars,memo_kind,sink,"
    "sink_group_id,fold_cut_task,fold_cut_turn,fold_cut_prefix,memo_cut_task,memo_cut_turn,"
    "memo_cut_prefix,forked_from_task_id,fork_point_completion_seq,output,completion_seq,"
    "tokens_prompt,tokens_completion,tokens_total,tokens_cached,last_total_tokens,last_fold_epoch,"
    "last_hist_len,last_turn_len,memo_seen_seq,last_prefix_tokens,redirect_stdout,redirect_stderr,"
    "redirect_append,redirect_stderr_append,todos,todo_sync_gap,updated_at) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
    "ON CONFLICT(task_id) DO UPDATE SET session_id=excluded.session_id,group_id=excluded.group_id,"
    "ord=excluded.ord,description=excluded.description,raw_input=excluded.raw_input,step=excluded.step,"
    "life=excluded.life,end_state=excluded.end_state,blocked_by=excluded.blocked_by,"
    "blocked_end=excluded.blocked_end,fail=excluded.fail,is_group_leader=excluded.is_group_leader,"
    "bg=excluded.bg,tool_only=excluded.tool_only,deferred_vars=excluded.deferred_vars,"
    "memo_kind=excluded.memo_kind,sink=excluded.sink,sink_group_id=excluded.sink_group_id,"
    "fold_cut_task=excluded.fold_cut_task,fold_cut_turn=excluded.fold_cut_turn,"
    "fold_cut_prefix=excluded.fold_cut_prefix,memo_cut_task=excluded.memo_cut_task,"
    "memo_cut_turn=excluded.memo_cut_turn,memo_cut_prefix=excluded.memo_cut_prefix,"
    "forked_from_task_id=excluded.forked_from_task_id,"
    "fork_point_completion_seq=excluded.fork_point_completion_seq,output=excluded.output,"
    "completion_seq=excluded.completion_seq,tokens_prompt=excluded.tokens_prompt,"
    "tokens_completion=excluded.tokens_completion,tokens_total=excluded.tokens_total,"
    "tokens_cached=excluded.tokens_cached,last_total_tokens=excluded.last_total_tokens,"
    "last_fold_epoch=excluded.last_fold_epoch,last_hist_len=excluded.last_hist_len,"
    "last_turn_len=excluded.last_turn_len,memo_seen_seq=excluded.memo_seen_seq,"
    "last_prefix_tokens=excluded.last_prefix_tokens,redirect_stdout=excluded.redirect_stdout,"
    "redirect_stderr=excluded.redirect_stderr,redirect_append=excluded.redirect_append,"
    "redirect_stderr_append=excluded.redirect_stderr_append,todos=excluded.todos,"
    "todo_sync_gap=excluded.todo_sync_gap,updated_at=excluded.updated_at";

struct task_bind
{
    const cJSON *tj;
    const char *session_id;
    int ord;
};

static void bind_task(sqlite3_stmt *st, void *ctx)
{
    struct task_bind *b = ctx;
    const cJSON *t = b->tj;
    int i = 1;
    bind_str(st, i++, js_str(t, "task_id"));
    bind_str(st, i++, b->session_id ? b->session_id : js_str(t, "session_id"));
    bind_str(st, i++, js_str(t, "group_id"));
    sqlite3_bind_int(st, i++, b->ord);
    bind_str(st, i++, js_str(t, "description") ? js_str(t, "description") : "");
    bind_str(st, i++, js_str(t, "raw_input"));
    bind_str(st, i++, js_str(t, "step"));
    bind_str(st, i++, js_str(t, "life"));
    bind_str(st, i++, js_str(t, "end"));
    bind_str(st, i++, js_str(t, "blocked_by"));
    bind_str(st, i++, js_str(t, "blocked_end"));
    bind_str(st, i++, js_str(t, "fail"));
    sqlite3_bind_int(st, i++, js_bool(t, "is_group_leader"));
    sqlite3_bind_int(st, i++, js_bool(t, "bg"));
    sqlite3_bind_int(st, i++, js_bool(t, "tool_only"));
    sqlite3_bind_int(st, i++, js_bool(t, "deferred_vars"));
    bind_str(st, i++, js_str(t, "memo_kind") ? js_str(t, "memo_kind") : "curate");
    bind_str(st, i++, js_str(t, "sink"));
    bind_str(st, i++, js_str(t, "sink_group_id"));
    bind_str(st, i++, js_str(t, "fold_cut_task"));
    bind_num(st, i++, js_num(t, "fold_cut_turn"));
    bind_num(st, i++, js_num(t, "fold_cut_prefix"));
    bind_str(st, i++, js_str(t, "memo_cut_task"));
    bind_num(st, i++, js_num(t, "memo_cut_turn"));
    bind_num(st, i++, js_num(t, "memo_cut_prefix"));
    bind_str(st, i++, js_str(t, "forked_from_task_id"));
    bind_num(st, i++, js_num(t, "fork_point_completion_seq"));
    bind_str(st, i++, js_str(t, "output"));
    bind_num(st, i++, js_num(t, "completion_seq"));
    bind_num(st, i++, js_num(t, "tokens_prompt"));
    bind_num(st, i++, js_num(t, "tokens_completion"));
    bind_num(st, i++, js_num(t, "tokens_total"));
    bind_num(st, i++, js_num(t, "tokens_cached"));
    bind_num(st, i++, js_num(t, "last_total_tokens"));
    bind_num(st, i++, js_num(t, "last_fold_epoch"));
    bind_num(st, i++, js_num(t, "last_hist_len"));
    bind_num(st, i++, js_num(t, "last_turn_len"));
    bind_num(st, i++, js_num(t, "memo_seen_seq"));
    bind_num(st, i++, js_num(t, "last_prefix_tokens"));
    bind_str(st, i++, js_str(t, "redirect_stdout"));
    bind_str(st, i++, js_str(t, "redirect_stderr"));
    sqlite3_bind_int(st, i++, js_bool(t, "redirect_append"));
    sqlite3_bind_int(st, i++, js_bool(t, "redirect_stderr_append"));
    {
        /* todos is an opaque JSON document: store its text, not a re-encode of it. */
        const cJSON *todos = cJSON_GetObjectItem((cJSON *)t, "todos");
        if (todos)
        {
            char *txt = cJSON_PrintUnformatted((cJSON *)todos);
            bind_str(st, i++, txt);
            cJSON_free(txt);
        }
        else
            sqlite3_bind_null(st, i++);
    }
    bind_num(st, i++, js_num(t, "todo_sync_gap"));
    sqlite3_bind_int64(st, i, (sqlite3_int64)time(NULL));
}

static const char *INSERT_DEP =
    "INSERT OR REPLACE INTO task_deps(target_task_id,source_task_id,type,completed,cancelled,"
    "injected,ord) VALUES(?,?,?,?,?,?,?)";

struct dep_bind
{
    const char *target;
    const cJSON *dj;
    int ord;
};

static void bind_dep(sqlite3_stmt *st, void *ctx)
{
    struct dep_bind *b = ctx;
    bind_str(st, 1, b->target);
    bind_str(st, 2, js_str(b->dj, "source_task_id"));
    bind_str(st, 3, js_str(b->dj, "type") ? js_str(b->dj, "type") : "JOIN");
    sqlite3_bind_int(st, 4, js_bool(b->dj, "completed"));
    sqlite3_bind_int(st, 5, js_bool(b->dj, "cancelled"));
    sqlite3_bind_int(st, 6, js_bool(b->dj, "injected"));
    sqlite3_bind_int(st, 7, b->ord);
}

static const char *INSERT_TOOL_CALL =
    "INSERT OR REPLACE INTO task_tool_calls(task_id,ord,call_id,name,args,status) VALUES(?,?,?,?,?,?)";

struct tool_call_bind
{
    const char *task_id;
    const cJSON *cj;
    int ord;
};

static void bind_tool_call(sqlite3_stmt *st, void *ctx)
{
    struct tool_call_bind *b = ctx;
    bind_str(st, 1, b->task_id);
    sqlite3_bind_int(st, 2, b->ord);
    bind_str(st, 3, js_str(b->cj, "call_id"));
    bind_str(st, 4, js_str(b->cj, "name"));
    {
        const cJSON *args = cJSON_GetObjectItem((cJSON *)b->cj, "args");
        if (args)
        {
            char *txt = cJSON_PrintUnformatted((cJSON *)args);
            bind_str(st, 5, txt);
            cJSON_free(txt);
        }
        else
            sqlite3_bind_null(st, 5);
    }
    bind_str(st, 6, js_str(b->cj, "status") ? js_str(b->cj, "status") : "pending");
}

static const char *INSERT_TURN =
    "INSERT INTO turns(task_id,turn_ord,folded_prefix,memo_prefix,is_active,msg_count,token_count) "
    "VALUES(?,?,?,?,?,?,?) "
    "ON CONFLICT(task_id,turn_ord) DO UPDATE SET folded_prefix=excluded.folded_prefix,"
    "memo_prefix=excluded.memo_prefix,is_active=excluded.is_active,"
    /* msg_count/token_count are NOT taken from the snapshot: the rows are the truth and
     * edc_msg_append keeps them current. Copying the snapshot value here would undo an
     * append that happened between serialization and this upsert. */
    "msg_count=(SELECT count(*) FROM messages WHERE task_id=excluded.task_id "
    "           AND turn_ord=excluded.turn_ord),"
    "token_count=(SELECT coalesce(sum(tokens),0) FROM messages WHERE task_id=excluded.task_id "
    "             AND turn_ord=excluded.turn_ord)";

struct turn_bind
{
    const char *task_id;
    int turn_ord;
    const cJSON *tj;
    int is_active;
};

static void bind_turn(sqlite3_stmt *st, void *ctx)
{
    struct turn_bind *b = ctx;
    bind_str(st, 1, b->task_id);
    sqlite3_bind_int(st, 2, b->turn_ord);
    bind_num(st, 3, js_num(b->tj, "folded_prefix"));
    bind_num(st, 4, js_num(b->tj, "memo_prefix"));
    sqlite3_bind_int(st, 5, b->is_active);

    /* The counts the runtime reads back. They arrive IN the snapshot (turn_to_json) and
     * are maintained on the live path by edc_msg_append's recount; the snapshot path must
     * not walk message bodies, which it no longer carries. */
    sqlite3_bind_int(st, 6, (int)js_num(b->tj, "msg_count"));
    sqlite3_bind_int64(st, 7, (sqlite3_int64)js_num(b->tj, "token_count"));
}

static const char *INSERT_MESSAGE =
    "INSERT OR REPLACE INTO messages(id,task_id,turn_ord,msg_ord,role,mid,ts,json,tokens) "
    "VALUES(?,?,?,?,?,?,?,?,?)";

struct message_bind
{
    const char *task_id;
    int turn_ord;
    const cJSON *mj;
    int msg_ord;
};

/* A message with no id gets a POSITIONAL one. It has to be deterministic: the store keys
 * on id, so a regenerated id would make every flush insert a second copy of the same
 * message. Only rows written by something other than edc_turn_add_message can lack one. */
static sds message_id_for(const cJSON *mj, const char *task_id, int turn_ord, int msg_ord)
{
    const char *own = js_str(mj, "id");
    if (own && *own)
        return sdsnew(own);
    return sdscatprintf(sdsempty(), "pos_%s_%d_%d", task_id ? task_id : "?", turn_ord, msg_ord);
}

static void bind_message(sqlite3_stmt *st, void *ctx)
{
    struct message_bind *b = ctx;
    sds id = message_id_for(b->mj, b->task_id, b->turn_ord, b->msg_ord);
    bind_str(st, 1, id);
    sdsfree(id);
    bind_str(st, 2, b->task_id);
    sqlite3_bind_int(st, 3, b->turn_ord);
    sqlite3_bind_int(st, 4, b->msg_ord);
    bind_str(st, 5, js_str(b->mj, "role"));
    bind_str(st, 6, js_str(b->mj, "mid"));
    sqlite3_bind_int64(st, 7, ts_from_display(js_str(b->mj, "ts")));
    {
        char *txt = cJSON_PrintUnformatted((cJSON *)b->mj);
        bind_str(st, 8, txt);
        cJSON_free(txt);
    }
    sqlite3_bind_int64(st, 9, edc_token_estimate_msg(b->mj));
}

/* ── flush bookkeeping ──────────────────────────────────────────────────────
 * Small helpers for the "upsert, then remove what disappeared" shape of the flush. */

static bool mark_seen_group(const char *group_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO flush_seen_group(group_id) VALUES(?)", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    bind_str(st, 1, group_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool mark_seen_task(const char *task_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO flush_seen_task(task_id) VALUES(?)", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    bind_str(st, 1, task_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool delete_group_fold_files(const char *session_id, const char *group_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM group_fold_files WHERE session_id=? AND group_id=?", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    bind_str(st, 1, session_id);
    bind_str(st, 2, group_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Turns at or beyond |keep| no longer exist for this task: drop them. Their messages go
 * with them through the cascade, which is correct -- a turn that was removed has no
 * conversation left to keep. */
static bool delete_turns_from(const char *task_id, int keep)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM turns WHERE task_id=? AND turn_ord>=?", -1, &st, NULL) !=
        SQLITE_OK)
        return false;
    bind_str(st, 1, task_id);
    sqlite3_bind_int(st, 2, keep);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Everything that was in the database but is not in this snapshot. Runs LAST, inside the
 * transaction, and only inside the session being flushed. */
static bool delete_stale(const char *session_id)
{
    /* Tasks and groups removed from the session. Cascades clean their turns/messages. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "DELETE FROM tasks WHERE session_id=? AND task_id NOT IN "
                           "(SELECT task_id FROM flush_seen_task)",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    bind_str(st, 1, session_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        return false;

    st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "DELETE FROM groups WHERE session_id=? AND group_id NOT IN "
                           "(SELECT group_id FROM flush_seen_group)",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    bind_str(st, 1, session_id);
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Write one turn (its row plus its messages) for |task_id|. */
static bool flush_turn(sqlite3 *db, const char *task_id, int turn_ord, const cJSON *tj, int is_active)
{
    struct turn_bind tb = {task_id, turn_ord, tj, is_active};
    if (!run_stmt(db, INSERT_TURN, "turn", bind_turn, &tb))
        return false;

    const cJSON *msgs = cJSON_GetObjectItem((cJSON *)tj, "messages");
    if (!msgs || !cJSON_IsArray(msgs))
        return true;
    int ord = 0;
    const cJSON *m = NULL;
    cJSON_ArrayForEach(m, (cJSON *)msgs)
    {
        struct message_bind mb = {task_id, turn_ord, m, ord++};
        if (!run_stmt(db, INSERT_MESSAGE, "message", bind_message, &mb))
            return false;
    }
    return true;
}

/* Write one task and everything hanging off it. */
static bool flush_task(sqlite3 *db, const char *session_id, const cJSON *tj, int ord)
{
    const char *task_id = js_str(tj, "task_id");
    if (!task_id || !*task_id)
        return true; /* nothing to key on: skip rather than fail the whole flush */

    struct task_bind tb = {tj, session_id, ord};
    if (!run_stmt(db, INSERT_TASK, "task", bind_task, &tb))
        return false;
    if (!mark_seen_task(task_id))
        return false;

    /* deps (outgoing edges) */
    const cJSON *deps = cJSON_GetObjectItem((cJSON *)tj, "deps");
    if (deps && cJSON_IsArray(deps))
    {
        int i = 0;
        const cJSON *d = NULL;
        cJSON_ArrayForEach(d, (cJSON *)deps)
        {
            struct dep_bind db_ind = {task_id, d, i++};
            if (!run_stmt(db, INSERT_DEP, "dep", bind_dep, &db_ind))
                return false;
        }
    }

    /* the incomplete-tool-call ledger */
    const cJSON *calls = cJSON_GetObjectItem((cJSON *)tj, "tool_calls");
    if (calls && cJSON_IsArray(calls))
    {
        int i = 0;
        const cJSON *c = NULL;
        cJSON_ArrayForEach(c, (cJSON *)calls)
        {
            struct tool_call_bind cb = {task_id, c, i++};
            if (!run_stmt(db, INSERT_TOOL_CALL, "tool_call", bind_tool_call, &cb))
                return false;
        }
    }

    /* committed turns, then the in-flight one. Active turns get turn_ord = count, which
     * is what the design doc calls the tail, so (task_id, turn_ord) stays unique. */
    const cJSON *turns = cJSON_GetObjectItem((cJSON *)tj, "turns");
    int turn_ord = 0;
    if (turns && cJSON_IsArray(turns))
    {
        const cJSON *t = NULL;
        cJSON_ArrayForEach(t, (cJSON *)turns)
        {
            if (!flush_turn(db, task_id, turn_ord, t, 0))
                return false;
            turn_ord++;
        }
    }
    const cJSON *active = cJSON_GetObjectItem((cJSON *)tj, "active_turn");
    if (active && cJSON_IsObject(active))
    {
        if (!flush_turn(db, task_id, turn_ord, active, 1))
            return false;
        turn_ord++;
    }

    /* Turns past the end no longer exist (a revert shortened the conversation). Dropping
     * them takes their message rows with them, which is the intent. */
    return delete_turns_from(task_id, turn_ord);
}

static bool flush_memo(sqlite3 *db, const char *session_id, const cJSON *memo)
{
    if (!memo)
        return true;

    /* The three counters live on the session row. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                               "UPDATE sessions SET memo_next_seq=?, memo_next_entry_id=?, "
                               "memo_pruned_before=? WHERE session_id=?", -1, &st, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)js_num(memo, "next_seq"));
        sqlite3_bind_int64(st, 2, (sqlite3_int64)js_num(memo, "next_entry_id"));
        sqlite3_bind_int64(st, 3, (sqlite3_int64)js_num(memo, "pruned_before"));
        sqlite3_bind_text(st, 4, session_id, -1, SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok)
            return false;
    }

    const cJSON *events = cJSON_GetObjectItem((cJSON *)memo, "events");
    if (events && cJSON_IsArray(events))
    {
        const cJSON *e = NULL;
        cJSON_ArrayForEach(e, (cJSON *)events)
        {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db,
                                   "INSERT OR REPLACE INTO memo_events(session_id,seq,ts,entry_id,"
                                   "kind,title,summary) VALUES(?,?,?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK)
                return false;
            bind_str(st, 1, session_id);
            bind_num(st, 2, js_num(e, "seq"));
            bind_num(st, 3, js_num(e, "ts"));
            bind_str(st, 4, js_str(e, "entry_id"));
            bind_str(st, 5, js_str(e, "kind"));
            bind_str(st, 6, js_str(e, "title"));
            bind_str(st, 7, js_str(e, "summary"));
            const bool ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok)
                return false;
        }
    }

    const cJSON *entries = cJSON_GetObjectItem((cJSON *)memo, "entries");
    if (entries && cJSON_IsArray(entries))
    {
        const cJSON *e = NULL;
        cJSON_ArrayForEach(e, (cJSON *)entries)
        {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db,
                                   "INSERT OR REPLACE INTO memo_entries(session_id,entry_id,title,"
                                   "summary,detail,sources,proposed_by,created_seq,updated_seq,"
                                   "created_at,updated_at,superseded) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                                   -1, &st, NULL) != SQLITE_OK)
                return false;
            bind_str(st, 1, session_id);
            bind_str(st, 2, js_str(e, "id"));
            bind_str(st, 3, js_str(e, "title"));
            bind_str(st, 4, js_str(e, "summary"));
            bind_str(st, 5, js_str(e, "detail"));
            bind_str(st, 6, js_str(e, "sources"));
            bind_str(st, 7, js_str(e, "proposed_by"));
            bind_num(st, 8, js_num(e, "created_seq"));
            bind_num(st, 9, js_num(e, "updated_seq"));
            bind_num(st, 10, js_num(e, "created_at"));
            bind_num(st, 11, js_num(e, "updated_at"));
            sqlite3_bind_int(st, 12, js_bool(e, "superseded"));
            const bool ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok)
                return false;
        }
    }
    return true;
}

/* Pages the pager has written since the connection opened (WAL mode: pages appended to
 * the -wal). Cumulative; the caller subtracts. Returns -1 when not measurable. */
static long long pages_written_now(long long *page_size_out)
{
    int cur = 0, hiwtr = 0;
    if (sqlite3_db_status(g_db, SQLITE_DBSTATUS_CACHE_WRITE, &cur, &hiwtr, 0) != SQLITE_OK)
        return -1;
    sqlite3_stmt *st = NULL;
    long long page_size = 0;
    if (sqlite3_prepare_v2(g_db, "PRAGMA page_size", -1, &st, NULL) == SQLITE_OK)
    {
        if (sqlite3_step(st) == SQLITE_ROW)
            page_size = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (page_size <= 0)
        return -1;
    if (page_size_out)
        *page_size_out = page_size;
    return (long long)cur;
}

bool edc_state_store_flush_measured(struct edc_session *session, struct edc_scheduler *sched,
                                    struct edc_state_flush_cost *out)
{
    if (out)
    {
        out->io_bytes = -1;
        out->rows = -1;
    }

    if (!g_db || !session || !session->session_id || !*session->session_id)
        return false;

    /* Sampled before BEGIN: the page counter and the change counter must bracket the whole
     * transaction, including the serialize that this function does first. */
    long long page_size = 0;
    const long long pages_before = out ? pages_written_now(&page_size) : -1;
    const int rows_before = out ? sqlite3_total_changes(g_db) : 0;

    cJSON *root = edc_state_serialize(session, sched);
    if (!root)
    {
        log_error("state_store: serialize failed for %s", session->session_id);
        return false;
    }

    const char *session_id = session->session_id;
    bool ok = false;

    if (!exec_sql(g_db, "BEGIN IMMEDIATE", "flush BEGIN"))
    {
        cJSON_Delete(root);
        return false;
    }

    /* Fork chains make tasks reference each other, so their FK checks cannot be
     * satisfied in arbitrary insert order. Deferring them to COMMIT is exactly what that
     * pragma is for; it resets at the end of the transaction. */
    if (!exec_sql(g_db, "PRAGMA defer_foreign_keys=ON", "flush defer_foreign_keys"))
        goto rollback;

    /* This used to be "DELETE FROM sessions WHERE session_id=?" -- one cascade that took
     * groups, tasks, turns, MESSAGES, deps, tool calls and fold files with it, then
     * re-inserted everything from the snapshot. Messages are no longer in the snapshot
     * (they are written a row at a time by edc_msg_append), so the delete would now wipe
     * the conversation. What follows instead is an UPSERT of every ancestor plus a
     * precise cleanup of the rows that genuinely disappeared. */
    {
        /* Seen-sets, so "not present in the snapshot any more" can be answered in SQL
         * without building an IN () list in C. TEMP tables live for the connection and
         * are emptied here, not re-created. */
        if (!exec_sql(g_db,
                      "CREATE TEMP TABLE IF NOT EXISTS flush_seen_group(group_id TEXT PRIMARY KEY);"
                      "CREATE TEMP TABLE IF NOT EXISTS flush_seen_task(task_id TEXT PRIMARY KEY);"
                      "DELETE FROM flush_seen_group;"
                      "DELETE FROM flush_seen_task;",
                      "flush seen-sets"))
            goto rollback;
    }

    {
        const cJSON *sj = cJSON_GetObjectItem(root, "session");
        struct session_bind sb = {sj, session_id};
        if (!run_stmt(g_db, INSERT_SESSION, "session row", bind_session, &sb))
            goto rollback;
    }

    {
        const cJSON *groups = cJSON_GetObjectItem(root, "groups");
        if (groups && cJSON_IsArray(groups))
        {
            int ord = 0;
            const cJSON *g = NULL;
            cJSON_ArrayForEach(g, (cJSON *)groups)
            {
                struct group_bind gb = {g, session_id, ord++};
                if (!run_stmt(g_db, INSERT_GROUP, "group", bind_group, &gb))
                    goto rollback;
                if (!mark_seen_group(js_str(g, "group_id")))
                    goto rollback;
                /* The file list is replaced wholesale: a path dropped from the group must
                 * not survive as a stale row. Its rows are not an ancestor of anything. */
                if (!delete_group_fold_files(session_id, js_str(g, "group_id")))
                    goto rollback;

                const cJSON *files = cJSON_GetObjectItem((cJSON *)g, "fold_files");
                if (files && cJSON_IsArray(files))
                {
                    int ford = 0;
                    const cJSON *f = NULL;
                    cJSON_ArrayForEach(f, (cJSON *)files)
                    {
                        sqlite3_stmt *st = NULL;
                        if (sqlite3_prepare_v2(g_db,
                                               "INSERT OR REPLACE INTO group_fold_files(session_id,"
                                               "group_id,ord,path) VALUES(?,?,?,?)", -1, &st, NULL) != SQLITE_OK)
                            goto rollback;
                        bind_str(st, 1, session_id);
                        bind_str(st, 2, js_str(g, "group_id"));
                        sqlite3_bind_int(st, 3, ford++);
                        bind_str(st, 4, cJSON_IsString((cJSON *)f) ? cJSON_GetStringValue((cJSON *)f) : NULL);
                        const bool okf = sqlite3_step(st) == SQLITE_DONE;
                        sqlite3_finalize(st);
                        if (!okf)
                            goto rollback;
                    }
                }
            }
        }
    }

    {
        const cJSON *tasks = cJSON_GetObjectItem(root, "tasks");
        if (tasks && cJSON_IsArray(tasks))
        {
            int ord = 0;
            const cJSON *t = NULL;
            cJSON_ArrayForEach(t, (cJSON *)tasks)
            {
                if (!flush_task(g_db, session_id, t, ord++))
                    goto rollback;
            }
        }
    }

    if (!flush_memo(g_db, session_id, cJSON_GetObjectItem(root, "session_memory")))
        goto rollback;

    if (!delete_stale(session_id))
        goto rollback;

    if (!exec_sql(g_db, "COMMIT", "flush COMMIT"))
        goto rollback;

    ok = true;
    goto done;

rollback:
    exec_sql(g_db, "ROLLBACK", "flush ROLLBACK");
done:
    if (out)
    {
        const long long pages_after = pages_written_now(NULL);
        out->io_bytes = (pages_before >= 0 && pages_after >= 0)
                            ? (pages_after - pages_before) * page_size
                            : -1;
        out->rows = (long long)(sqlite3_total_changes(g_db) - rows_before);
    }
    cJSON_Delete(root);
    return ok;
}

bool edc_state_store_flush(struct edc_session *session, struct edc_scheduler *sched)
{
    /* The cost measurement costs two stat() calls, so it is off unless asked for. */
    return edc_state_store_flush_measured(session, sched, NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  load: columns -> tree -> edc_state_deserialize
 * ══════════════════════════════════════════════════════════════════════════ */

static void add_str(cJSON *o, const char *key, const char *val)
{
    if (val)
        cJSON_AddStringToObject(o, key, val);
}

static const char *col_str(sqlite3_stmt *st, int i)
{
    return sqlite3_column_type(st, i) == SQLITE_NULL ? NULL : (const char *)sqlite3_column_text(st, i);
}

static void add_display_ts(cJSON *o, const char *key, sqlite3_stmt *st, int i)
{
    if (sqlite3_column_type(st, i) == SQLITE_NULL)
        return;
    const sqlite3_int64 t = sqlite3_column_int64(st, i);
    if (t <= 0)
        return; /* 0 = unknown: omit, so the reader sees the same absence it always did */
    sds disp = ts_to_display(t);
    cJSON_AddStringToObject(o, key, disp ? disp : "");
    sdsfree(disp);
}

static void add_num_or_omit(cJSON *o, const char *key, sqlite3_stmt *st, int i)
{
    if (sqlite3_column_type(st, i) != SQLITE_NULL)
        cJSON_AddNumberToObject(o, key, (double)sqlite3_column_int64(st, i));
}

/* ── messages of one turn ─────────────────────────────────────────────────── */

static cJSON *load_turn_messages(const char *task_id, int turn_ord)
{
    cJSON *arr = cJSON_CreateArray();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT json FROM messages WHERE task_id=? AND turn_ord=? "
                                 "ORDER BY msg_ord", -1, &st, NULL) != SQLITE_OK)
        return arr;
    sqlite3_bind_text(st, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, turn_ord);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const char *json = col_str(st, 0);
        if (!json)
            continue;
        cJSON *msg = cJSON_Parse(json);
        if (msg)
            cJSON_AddItemToArray(arr, msg);
    }
    sqlite3_finalize(st);
    return arr;
}

/* ── one task, with its child collections ─────────────────────────────────── */

struct load_task_ctx
{
    cJSON *task;
};

/* Build the task object plus deps, tool_calls, turns and active_turn. */
static cJSON *load_task_row(sqlite3_stmt *st)
{
    cJSON *tj = cJSON_CreateObject();
    const char *task_id = col_str(st, 0);
    if (!task_id)
    {
        cJSON_Delete(tj);
        return NULL;
    }

    cJSON_AddStringToObject(tj, "task_id", task_id);
    add_str(tj, "group_id", col_str(st, 1));
    add_str(tj, "session_id", col_str(st, 2));
    add_str(tj, "description", col_str(st, 3));
    add_str(tj, "raw_input", col_str(st, 4));
    add_str(tj, "step", col_str(st, 5));
    add_str(tj, "life", col_str(st, 6));
    add_str(tj, "end", col_str(st, 7));
    add_str(tj, "blocked_by", col_str(st, 8));
    add_str(tj, "blocked_end", col_str(st, 9));
    add_str(tj, "fail", col_str(st, 10));
    cJSON_AddBoolToObject(tj, "is_group_leader", sqlite3_column_int(st, 11) != 0);
    cJSON_AddBoolToObject(tj, "bg", sqlite3_column_int(st, 12) != 0);
    cJSON_AddBoolToObject(tj, "tool_only", sqlite3_column_int(st, 13) != 0);
    cJSON_AddBoolToObject(tj, "deferred_vars", sqlite3_column_int(st, 14) != 0);
    add_str(tj, "memo_kind", col_str(st, 15));
    add_str(tj, "sink", col_str(st, 16));
    add_str(tj, "sink_group_id", col_str(st, 17));
    add_str(tj, "fold_cut_task", col_str(st, 18));
    add_num_or_omit(tj, "fold_cut_turn", st, 19);
    add_num_or_omit(tj, "fold_cut_prefix", st, 20);
    add_str(tj, "memo_cut_task", col_str(st, 21));
    add_num_or_omit(tj, "memo_cut_turn", st, 22);
    add_num_or_omit(tj, "memo_cut_prefix", st, 23);
    add_str(tj, "forked_from_task_id", col_str(st, 24));
    cJSON_AddNumberToObject(tj, "fork_point_completion_seq", (double)sqlite3_column_int64(st, 25));
    add_str(tj, "output", col_str(st, 26));
    cJSON_AddNumberToObject(tj, "completion_seq", (double)sqlite3_column_int64(st, 27));
    cJSON_AddNumberToObject(tj, "tokens_prompt", (double)sqlite3_column_int64(st, 28));
    cJSON_AddNumberToObject(tj, "tokens_completion", (double)sqlite3_column_int64(st, 29));
    cJSON_AddNumberToObject(tj, "tokens_total", (double)sqlite3_column_int64(st, 30));
    cJSON_AddNumberToObject(tj, "tokens_cached", (double)sqlite3_column_int64(st, 31));
    cJSON_AddNumberToObject(tj, "last_total_tokens", (double)sqlite3_column_int64(st, 32));
    cJSON_AddNumberToObject(tj, "last_fold_epoch", (double)sqlite3_column_int64(st, 33));
    cJSON_AddNumberToObject(tj, "last_hist_len", (double)sqlite3_column_int64(st, 34));
    cJSON_AddNumberToObject(tj, "last_turn_len", (double)sqlite3_column_int64(st, 35));
    cJSON_AddNumberToObject(tj, "memo_seen_seq", (double)sqlite3_column_int64(st, 36));
    cJSON_AddNumberToObject(tj, "last_prefix_tokens", (double)sqlite3_column_int64(st, 37));
    add_str(tj, "redirect_stdout", col_str(st, 38));
    add_str(tj, "redirect_stderr", col_str(st, 39));
    cJSON_AddBoolToObject(tj, "redirect_append", sqlite3_column_int(st, 40) != 0);
    cJSON_AddBoolToObject(tj, "redirect_stderr_append", sqlite3_column_int(st, 41) != 0);
    {
        const char *todos = col_str(st, 42);
        if (todos)
        {
            cJSON *parsed = cJSON_Parse(todos);
            if (parsed)
                cJSON_AddItemToObject(tj, "todos", parsed);
        }
    }
    cJSON_AddNumberToObject(tj, "todo_sync_gap", (double)sqlite3_column_int64(st, 43));

    /* deps */
    {
        cJSON *deps = cJSON_CreateArray();
        sqlite3_stmt *d = NULL;
        if (sqlite3_prepare_v2(g_db, "SELECT source_task_id,type,completed,cancelled,injected "
                                     "FROM task_deps WHERE target_task_id=? ORDER BY ord",
                               -1, &d, NULL) == SQLITE_OK)
        {
            sqlite3_bind_text(d, 1, task_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(d) == SQLITE_ROW)
            {
                cJSON *o = cJSON_CreateObject();
                add_str(o, "source_task_id", col_str(d, 0));
                add_str(o, "type", col_str(d, 1));
                cJSON_AddBoolToObject(o, "completed", sqlite3_column_int(d, 2) != 0);
                cJSON_AddBoolToObject(o, "cancelled", sqlite3_column_int(d, 3) != 0);
                cJSON_AddBoolToObject(o, "injected", sqlite3_column_int(d, 4) != 0);
                cJSON_AddItemToArray(deps, o);
            }
        }
        sqlite3_finalize(d);
        cJSON_AddItemToObject(tj, "deps", deps);
    }

    /* the resume ledger */
    {
        sqlite3_stmt *c = NULL;
        cJSON *calls = NULL;
        if (sqlite3_prepare_v2(g_db, "SELECT call_id,name,args,status FROM task_tool_calls "
                                     "WHERE task_id=? ORDER BY ord", -1, &c, NULL) == SQLITE_OK)
        {
            sqlite3_bind_text(c, 1, task_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(c) == SQLITE_ROW)
            {
                if (!calls)
                    calls = cJSON_CreateArray();
                cJSON *o = cJSON_CreateObject();
                add_str(o, "call_id", col_str(c, 0));
                add_str(o, "name", col_str(c, 1));
                {
                    const char *args = col_str(c, 2);
                    if (args)
                    {
                        cJSON *parsed = cJSON_Parse(args);
                        if (parsed)
                            cJSON_AddItemToObject(o, "args", parsed);
                    }
                }
                add_str(o, "status", col_str(c, 3));
                cJSON_AddItemToArray(calls, o);
            }
        }
        sqlite3_finalize(c);
        if (calls)
            cJSON_AddItemToObject(tj, "tool_calls", calls);
    }

    /* turns: committed ones, plus the active one as its own key */
    {
        cJSON *turns = cJSON_CreateArray();
        cJSON *active = NULL;
        sqlite3_stmt *t = NULL;
        if (sqlite3_prepare_v2(g_db, "SELECT turn_ord,folded_prefix,memo_prefix,is_active,"
                                     "msg_count,token_count "
                                     "FROM turns WHERE task_id=? ORDER BY turn_ord",
                               -1, &t, NULL) == SQLITE_OK)
        {
            sqlite3_bind_text(t, 1, task_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(t) == SQLITE_ROW)
            {
                cJSON *o = cJSON_CreateObject();
                /* The shape only: the bodies stay in the rows and are read on demand. */
                cJSON_AddNumberToObject(o, "msg_count", (double)sqlite3_column_int(t, 4));
                cJSON_AddNumberToObject(o, "token_count", (double)sqlite3_column_int64(t, 5));
                cJSON_AddNumberToObject(o, "folded_prefix", (double)sqlite3_column_int(t, 1));
                cJSON_AddNumberToObject(o, "memo_prefix", (double)sqlite3_column_int(t, 2));
                if (sqlite3_column_int(t, 3) != 0)
                    active = o;
                else
                    cJSON_AddItemToArray(turns, o);
            }
        }
        sqlite3_finalize(t);
        cJSON_AddItemToObject(tj, "turns", turns);
        if (active)
            cJSON_AddItemToObject(tj, "active_turn", active);
    }

    return tj;
}

/* The column list has to match the index order used by load_task_row. Kept in one place
 * so the two cannot drift silently. */
static const char *SELECT_TASK_COLS =
    "task_id,group_id,session_id,description,raw_input,step,life,end_state,blocked_by,"
    "blocked_end,fail,is_group_leader,bg,tool_only,deferred_vars,memo_kind,sink,sink_group_id,"
    "fold_cut_task,fold_cut_turn,fold_cut_prefix,memo_cut_task,memo_cut_turn,memo_cut_prefix,"
    "forked_from_task_id,fork_point_completion_seq,output,completion_seq,tokens_prompt,"
    "tokens_completion,tokens_total,tokens_cached,last_total_tokens,last_fold_epoch,last_hist_len,"
    "last_turn_len,memo_seen_seq,last_prefix_tokens,redirect_stdout,redirect_stderr,redirect_append,"
    "redirect_stderr_append,todos,todo_sync_gap";

static bool session_exists(const char *session_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT 1 FROM sessions WHERE session_id=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

bool edc_state_store_load(struct edc_session *session, struct edc_scheduler *sched)
{
    if (!g_db || !session || !session->session_id || !*session->session_id)
        return false;

    const char *session_id = session->session_id;
    if (!session_exists(session_id))
        return false; /* a brand-new session: nothing on disk, not a failure */

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", edc_state_schema_version());

    /* ── session block ── */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "SELECT session_id,name,created_at,last_active,workdir,active_task_id,"
                               "foreground_group_id,next_completion_seq,memo_next_seq,"
                               "memo_next_entry_id,memo_pruned_before FROM sessions WHERE session_id=?",
                               -1, &st, NULL) != SQLITE_OK)
        {
            cJSON_Delete(root);
            return false;
        }
        sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_ROW)
        {
            sqlite3_finalize(st);
            cJSON_Delete(root);
            return false;
        }
        cJSON *sj = cJSON_CreateObject();
        add_str(sj, "session_id", col_str(st, 0));
        add_str(sj, "name", col_str(st, 1));
        add_display_ts(sj, "created_at", st, 2);
        add_display_ts(sj, "last_active", st, 3);
        add_str(sj, "workdir", col_str(st, 4));
        add_str(sj, "active_task_id", col_str(st, 5));
        add_str(sj, "foreground_group_id", col_str(st, 6));
        cJSON_AddNumberToObject(sj, "next_completion_seq", (double)sqlite3_column_int64(st, 7));
        cJSON_AddItemToObject(root, "session", sj);

        /* The three memo counters belong to the session_memory block, not the session. */
        cJSON *memo = cJSON_CreateObject();
        cJSON_AddNumberToObject(memo, "next_seq", (double)sqlite3_column_int64(st, 8));
        cJSON_AddNumberToObject(memo, "next_entry_id", (double)sqlite3_column_int64(st, 9));
        cJSON_AddNumberToObject(memo, "pruned_before", (double)sqlite3_column_int64(st, 10));
        cJSON_AddItemToObject(root, "session_memory", memo);
        sqlite3_finalize(st);
    }

    /* ── groups ── */
    {
        cJSON *groups = cJSON_CreateArray();
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, "SELECT group_id,leader_task_id,is_orphaned,kind,memory,"
                                     "fold_epoch,memory_created_at,memory_updated_at,"
                                     "memory_covers_to FROM groups WHERE session_id=? ORDER BY ord",
                               -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                cJSON *gj = cJSON_CreateObject();
                const char *group_id = col_str(st, 0);
                add_str(gj, "group_id", group_id);
                add_str(gj, "leader_task_id", col_str(st, 1));
                cJSON_AddBoolToObject(gj, "is_orphaned", sqlite3_column_int(st, 2) != 0);
                add_str(gj, "kind", col_str(st, 3));
                add_str(gj, "memory", col_str(st, 4));
                cJSON_AddNumberToObject(gj, "fold_epoch", (double)sqlite3_column_int64(st, 5));
                cJSON_AddNumberToObject(gj, "memory_created_at", (double)sqlite3_column_int64(st, 6));
                cJSON_AddNumberToObject(gj, "memory_updated_at", (double)sqlite3_column_int64(st, 7));
                cJSON_AddNumberToObject(gj, "memory_covers_to", (double)sqlite3_column_int64(st, 8));

                /* fold file list */
                {
                    sqlite3_stmt *f = NULL;
                    cJSON *files = NULL;
                    if (group_id &&
                        sqlite3_prepare_v2(g_db, "SELECT path FROM group_fold_files WHERE session_id=? "
                                                 "AND group_id=? ORDER BY ord", -1, &f, NULL) == SQLITE_OK)
                    {
                        sqlite3_bind_text(f, 1, session_id, -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(f, 2, group_id, -1, SQLITE_TRANSIENT);
                        while (sqlite3_step(f) == SQLITE_ROW)
                        {
                            const char *path = col_str(f, 0);
                            if (!path)
                                continue;
                            if (!files)
                                files = cJSON_CreateArray();
                            cJSON_AddItemToArray(files, cJSON_CreateString(path));
                        }
                    }
                    sqlite3_finalize(f);
                    if (files)
                        cJSON_AddItemToObject(gj, "fold_files", files);
                }

                /* members, in group order */
                {
                    sqlite3_stmt *m = NULL;
                    cJSON *members = cJSON_CreateArray();
                    if (group_id &&
                        sqlite3_prepare_v2(g_db, "SELECT task_id FROM tasks WHERE session_id=? AND "
                                                 "group_id=? ORDER BY ord", -1, &m, NULL) == SQLITE_OK)
                    {
                        sqlite3_bind_text(m, 1, session_id, -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(m, 2, group_id, -1, SQLITE_TRANSIENT);
                        while (sqlite3_step(m) == SQLITE_ROW)
                        {
                            const char *tid = col_str(m, 0);
                            cJSON_AddItemToArray(members, cJSON_CreateString(tid ? tid : ""));
                        }
                    }
                    sqlite3_finalize(m);
                    cJSON_AddItemToObject(gj, "member_task_ids", members);
                }

                cJSON_AddItemToArray(groups, gj);
            }
        }
        sqlite3_finalize(st);
        cJSON_AddItemToObject(root, "groups", groups);
    }

    /* ── tasks ── */
    {
        cJSON *tasks = cJSON_CreateArray();
        char sql[1024];
        snprintf(sql, sizeof(sql), "SELECT %s FROM tasks WHERE session_id=? ORDER BY group_id, ord",
                 SELECT_TASK_COLS);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                cJSON *tj = load_task_row(st);
                if (tj)
                    cJSON_AddItemToArray(tasks, tj);
            }
        }
        sqlite3_finalize(st);
        cJSON_AddItemToObject(root, "tasks", tasks);
    }

    /* ── memo events + entries ── */
    {
        cJSON *memo = cJSON_GetObjectItem(root, "session_memory");
        {
            cJSON *events = cJSON_CreateArray();
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(g_db, "SELECT seq,ts,entry_id,kind,title,summary FROM memo_events "
                                         "WHERE session_id=? ORDER BY seq", -1, &st, NULL) == SQLITE_OK)
            {
                sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(st) == SQLITE_ROW)
                {
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddNumberToObject(o, "seq", (double)sqlite3_column_int64(st, 0));
                    cJSON_AddNumberToObject(o, "ts", (double)sqlite3_column_int64(st, 1));
                    add_str(o, "entry_id", col_str(st, 2));
                    add_str(o, "kind", col_str(st, 3));
                    add_str(o, "title", col_str(st, 4));
                    add_str(o, "summary", col_str(st, 5));
                    cJSON_AddItemToArray(events, o);
                }
            }
            sqlite3_finalize(st);
            cJSON_AddItemToObject(memo, "events", events);
        }
        {
            cJSON *entries = cJSON_CreateArray();
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(g_db, "SELECT entry_id,title,summary,detail,sources,proposed_by,"
                                         "created_seq,updated_seq,created_at,updated_at,superseded "
                                         "FROM memo_entries WHERE session_id=? ORDER BY entry_id",
                                   -1, &st, NULL) == SQLITE_OK)
            {
                sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
                while (sqlite3_step(st) == SQLITE_ROW)
                {
                    cJSON *o = cJSON_CreateObject();
                    add_str(o, "id", col_str(st, 0));
                    add_str(o, "title", col_str(st, 1));
                    add_str(o, "summary", col_str(st, 2));
                    add_str(o, "detail", col_str(st, 3));
                    add_str(o, "sources", col_str(st, 4));
                    add_str(o, "proposed_by", col_str(st, 5));
                    cJSON_AddNumberToObject(o, "created_seq", (double)sqlite3_column_int64(st, 6));
                    cJSON_AddNumberToObject(o, "updated_seq", (double)sqlite3_column_int64(st, 7));
                    cJSON_AddNumberToObject(o, "created_at", (double)sqlite3_column_int64(st, 8));
                    cJSON_AddNumberToObject(o, "updated_at", (double)sqlite3_column_int64(st, 9));
                    cJSON_AddBoolToObject(o, "superseded", sqlite3_column_int(st, 10) != 0);
                    cJSON_AddItemToArray(entries, o);
                }
            }
            sqlite3_finalize(st);
            cJSON_AddItemToObject(memo, "entries", entries);
        }
    }

    /* Hand the assembled tree to the ONE implementation of the restart normalization. */
    const bool ok = edc_state_deserialize(root, session, sched);
    cJSON_Delete(root);
    if (!ok)
        log_error("state_store: deserialize failed for %s", session_id);
    return ok;
}

bool edc_state_store_has_session(const char *session_id)
{
    if (!g_db || !session_id || !*session_id)
        return false;
    return session_exists(session_id);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Message rows: the runtime read/write path
 *
 *  These are deliberately NOT built on edc_state_serialize(): that function exists to
 *  produce a whole-session snapshot, and using it here is exactly what this stage
 *  removes. A message is a row; appending one is one INSERT.
 * ══════════════════════════════════════════════════════════════════════════ */

bool edc_state_ensure_turn(const char *session_id, const char *group_id, const char *task_id,
                           int turn_ord)
{
    if (!g_db || !task_id || !*task_id)
        return false;
    /* A turn built without a session (tests, and any caller that only carries a task id)
     * still needs an ancestor row for the FK chain. One shared placeholder keeps that
     * possible without pretending it is a real session; production always passes one. */
    if (!session_id || !*session_id)
        session_id = "sess-default";

    static const char *const steps[] = {
        /* Column list and defaults match the DDL: only NOT NULL columns are given a value,
         * everything else keeps its default until the metadata flush fills it in. */
        "INSERT OR IGNORE INTO sessions(session_id,name,updated_at) VALUES(?,'',0)",
        "INSERT OR IGNORE INTO groups(session_id,group_id) VALUES(?,?)",
        "INSERT OR IGNORE INTO tasks(task_id,session_id,group_id,updated_at) VALUES(?,?,?,0)",
        "INSERT OR IGNORE INTO turns(task_id,turn_ord) VALUES(?,?)",
    };

    for (int i = 0; i < 4; i++)
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, steps[i], -1, &st, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_text(st, 1, (i == 0 || i == 1) ? session_id : task_id, -1, SQLITE_TRANSIENT);
        if (i == 1)
            sqlite3_bind_text(st, 2, (group_id && *group_id) ? group_id : "", -1, SQLITE_TRANSIENT);
        else if (i == 2)
        {
            sqlite3_bind_text(st, 2, session_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, (group_id && *group_id) ? group_id : "", -1, SQLITE_TRANSIENT);
        }
        else if (i == 3)
            sqlite3_bind_int(st, 2, turn_ord);

        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok)
            return false;
    }
    return true;
}

/* Recompute the aggregates the runtime reads instead of walking rows:
 * turns.msg_count / turns.token_count. Called after any message insert or delete, so the
 * two numbers can never drift from the rows they summarize. Indexed range scan, no table
 * walk. */
static void turn_recount(const char *task_id, int turn_ord)
{
    if (!g_db || !task_id || !*task_id)
        return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "UPDATE turns SET "
                           "  msg_count   = (SELECT count(*) FROM messages WHERE task_id=?1 AND turn_ord=?2), "
                           "  token_count = (SELECT coalesce(sum(tokens),0) FROM messages "
                           "                 WHERE task_id=?1 AND turn_ord=?2) "
                           "WHERE task_id=?1 AND turn_ord=?2",
                           -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, turn_ord);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

bool edc_msg_append(const char *task_id, int turn_ord, int msg_ord, const struct cJSON *msg,
                    long long tokens)
{
    if (!g_db || !task_id || !*task_id || !msg || msg_ord < 0)
        return false;

    char *txt = cJSON_PrintUnformatted((cJSON *)msg);
    if (!txt)
        return false;

    const cJSON *role = cJSON_GetObjectItem((cJSON *)msg, "role");
    const cJSON *mid = cJSON_GetObjectItem((cJSON *)msg, "id");
    const cJSON *ts = cJSON_GetObjectItem((cJSON *)msg, "ts");

    sqlite3_stmt *st = NULL;
    /* OR REPLACE on the natural key: re-appending the same position (a replayed event, a
     * retry) overwrites rather than duplicating. The row's `id` primary key is rewritten
     * too, so a changed message cannot leave two identities for one position. */
    const char *sql = "INSERT OR REPLACE INTO messages(id,task_id,turn_ord,msg_ord,role,mid,ts,json,tokens) "
                      "VALUES(?,?,?,?,?,?,?,?,?)";
    bool ok = false;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK)
    {
        const char *id = cJSON_GetStringValue((cJSON *)mid);
        sds fallback = NULL;
        if (!id || !*id)
        {
            /* No identity on the message: a deterministic positional one (same rule the
             * snapshot writer uses, see bind_message). */
            fallback = sdscatprintf(sdsempty(), "%s#%d.%d", task_id, turn_ord, msg_ord);
            id = fallback;
        }
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, turn_ord);
        sqlite3_bind_int(st, 4, msg_ord);
        bind_str(st, 5, cJSON_GetStringValue((cJSON *)role));
        bind_str(st, 6, cJSON_GetStringValue((cJSON *)mid));
        bind_num(st, 7, ts ? strtod(cJSON_GetStringValue((cJSON *)ts), NULL) : 0);
        sqlite3_bind_text(st, 8, txt, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 9, (sqlite3_int64)tokens);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (fallback)
            sdsfree(fallback);
    }
    cJSON_free(txt);
    if (ok)
        turn_recount(task_id, turn_ord);
    return ok;
}

/* ── Message rows end ─────────────────────────────────────────────────────── */

struct cJSON *edc_msg_get(const char *task_id, int turn_ord, int msg_ord)
{
    if (!g_db || !task_id || !*task_id || msg_ord < 0)
        return NULL;

    sqlite3_stmt *st = NULL;
    struct cJSON *out = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT json FROM messages WHERE task_id=? AND turn_ord=? AND msg_ord=?",
                           -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, turn_ord);
        sqlite3_bind_int(st, 3, msg_ord);
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            const char *json = col_str(st, 0);
            if (json)
                out = cJSON_Parse(json);
        }
        sqlite3_finalize(st);
    }
    return out;
}

int edc_msg_turn_len(const char *task_id, int turn_ord)
{
    if (!g_db || !task_id || !*task_id)
        return 0;

    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT count(*) FROM messages WHERE task_id=? AND turn_ord=?", -1, &st,
                           NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, turn_ord);
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

long long edc_msg_turn_tokens_from(const char *task_id, int turn_ord, int from)
{
    if (!g_db || !task_id || !*task_id)
        return -1;

    sqlite3_stmt *st = NULL;
    long long total = 0;
    if (sqlite3_prepare_v2(g_db,
                           "SELECT coalesce(sum(tokens),0) FROM messages "
                           "WHERE task_id=? AND turn_ord=? AND msg_ord>=?",
                           -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, turn_ord);
        sqlite3_bind_int(st, 3, from > 0 ? from : 0);
        if (sqlite3_step(st) == SQLITE_ROW)
            total = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return total;
}

static bool msg_run_delete(const char *sql, const char *task_id, int turn_ord, int arg3)
{
    if (!g_db || !task_id || !*task_id)
        return false;
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, turn_ord);
        if (arg3 >= 0)
            sqlite3_bind_int(st, 3, arg3);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    return ok;
}

bool edc_msg_turn_truncate(const char *task_id, int turn_ord, int new_len)
{
    const bool ok = msg_run_delete("DELETE FROM messages WHERE task_id=? AND turn_ord=? AND msg_ord>=?",
                                   task_id, turn_ord, new_len < 0 ? 0 : new_len);
    if (ok)
        turn_recount(task_id, turn_ord);
    return ok;
}

bool edc_msg_turn_clear(const char *task_id, int turn_ord)
{
    const bool ok = msg_run_delete("DELETE FROM messages WHERE task_id=? AND turn_ord=?", task_id,
                                   turn_ord, -1);
    if (ok)
        turn_recount(task_id, turn_ord);
    return ok;
}

bool edc_msg_turn_move(const char *old_task, int old_ord, const char *new_session,
                       const char *new_group, const char *new_task, int new_ord)
{
    if (!g_db || !old_task || !new_task)
        return false;
    if (strcmp(old_task, new_task) == 0 && old_ord == new_ord)
        return true; /* already where it belongs: nothing to move */

    /* The destination's ancestor chain (and its turn row) must exist first. */
    if (!edc_state_ensure_turn(new_session, new_group, new_task, new_ord))
        return false;

    /* Carry the watermarks across before the old row goes. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "UPDATE turns SET folded_prefix=(SELECT folded_prefix FROM turns "
                               "  WHERE task_id=?3 AND turn_ord=?4), "
                               "  memo_prefix=(SELECT memo_prefix FROM turns "
                               "  WHERE task_id=?3 AND turn_ord=?4) "
                               "WHERE task_id=?1 AND turn_ord=?2",
                               -1, &st, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_text(st, 1, new_task, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, new_ord);
        sqlite3_bind_text(st, 3, old_task, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, old_ord);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    /* Messages FIRST: while they still point at the old turn row, changing the turns key
     * would violate messages' foreign key. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "UPDATE OR REPLACE messages SET task_id=?, turn_ord=? "
                               "WHERE task_id=? AND turn_ord=?",
                               -1, &st, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_text(st, 1, new_task, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, new_ord);
        sqlite3_bind_text(st, 3, old_task, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, old_ord);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok)
            return false;
    }

    /* Now nothing references the old turn row: drop it and recount the new one. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, "DELETE FROM turns WHERE task_id=? AND turn_ord=?", -1, &st,
                               NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_text(st, 1, old_task, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, old_ord);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    turn_recount(new_task, new_ord);
    return true;
}

bool edc_msg_span_load(const char *task_id, int turn_ord, int from, int to, struct edc_msg_span *out)
{
    if (!out)
        return false;
    out->items = NULL;
    out->n = 0;

    if (!g_db || !task_id || !*task_id)
        return false;
    if (from < 0)
        from = 0;

    sqlite3_stmt *st = NULL;
    const char *sql = (to < 0)
                          ? "SELECT msg_ord,json FROM messages WHERE task_id=? AND turn_ord=? AND msg_ord>=? "
                            "ORDER BY msg_ord"
                          : "SELECT msg_ord,json FROM messages WHERE task_id=? AND turn_ord=? "
                            "AND msg_ord>=? AND msg_ord<? ORDER BY msg_ord";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_text(st, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, turn_ord);
    sqlite3_bind_int(st, 3, from);
    if (to >= 0)
        sqlite3_bind_int(st, 4, to);

    int cap = 0;
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const int ord = sqlite3_column_int(st, 0);
        /* Keep indices aligned with msg_ord: fill any hole with NULL. A gap only appears
         * if a message was deleted out of the middle, which truncate does not do. */
        const int want = (ord - from) + 1;
        if (want > cap)
        {
            const int ncap = want * 2;
            struct cJSON **grown = realloc(out->items, (size_t)ncap * sizeof(*grown));
            if (!grown)
                break;
            for (int k = cap; k < ncap; k++)
                grown[k] = NULL;
            out->items = grown;
            cap = ncap;
        }
        const char *json = col_str(st, 1);
        out->items[want - 1] = json ? cJSON_Parse(json) : NULL;
        if (want > n)
            n = want;
    }
    sqlite3_finalize(st);
    out->n = n;
    return true;
}

void edc_msg_span_free(struct edc_msg_span *span)
{
    if (!span || !span->items)
        return;
    for (int i = 0; i < span->n; i++)
        if (span->items[i])
            cJSON_Delete(span->items[i]);
    free(span->items);
    span->items = NULL;
    span->n = 0;
}

bool edc_state_store_forget(const char *session_id)
{
    if (!g_db || !session_id || !*session_id)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM sessions WHERE session_id=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, session_id, -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

char *edc_state_store_session_for_task(const char *task_id)
{
    if (!g_db || !task_id || !*task_id)
        return NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT session_id FROM tasks WHERE task_id=?", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, task_id, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        const char *sid = col_str(st, 0);
        if (sid)
            out = strdup(sid);
    }
    sqlite3_finalize(st);
    return out;
}
