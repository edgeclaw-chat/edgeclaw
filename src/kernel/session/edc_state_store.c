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

#include "edc_scheduler.h"
#include "edc_state_migrate.h"
#include "edc_state_store.h"
#include "edc_version.h"

#include "edc_config.h"
#include "edc_datetime_utils.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_path.h"
#include "edc_session.h"
#include "edc_session_memory.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "edc_turn.h"
#include "edc_token_estimate.h"
#include "utlist_fix.h"
#include "cJSON.h"
#include "log.h"
#include "sds.h"
#include "sqlite3.h"

#include <inttypes.h> /* PRId64 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

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

/* Identity of the database file this connection was opened on. If the file is later
 * removed or replaced (a test that wipes its workspace, or someone deleting the database
 * to recreate it), the open handle would keep writing into an unlinked inode and every
 * subsequent read would see nothing. Comparing the identity before each use turns that
 * silent split-brain into a reopen. dev/ino are 0 on platforms without st_ino, where
 * exists_if_same() falls back to "the path still exists". */
static uint64_t g_db_dev;
static uint64_t g_db_ino;

/* ── Schema ─────────────────────────────────────────────────────────────────
 * Transcribed from design doc §4. Three rules the DDL follows:
 *   1. every structure in the old session.json has a column (no whole-subtree blobs),
 *      otherwise neither queries nor incremental writes are possible;
 *   2. messages keep the wire fields in one `body` JSON and everything the runtime asks
 *      about in columns (see kStoreOnlyMsgKeys). The split has exactly one source of truth
 *      per value: a field is either a column or in `body`, never both -- storing a value in
 *      two places is what let `ts` drift between an integer column and a display string in
 *      v2;
 *   3. ordering is explicit (`ord`), never rowid: groups are a linked list and turns are
 *      a utarray, so insertion order is not something SQLite can infer.
 *
 * `end_state` is not called `end`: SQL keyword. There is no `extra` escape hatch: an
 * unused TEXT column is not a plan for the future, it is a place for data to accumulate
 * that nothing reads (both were dropped in the v3 review).
 *
 * EVERY timestamp column is INTEGER: seconds since the Unix epoch. That is the one
 * convention the in-memory code already uses (edc_datetime_now() is time(NULL)), so the
 * database does not invent a second one. Two conversions happen at the edges and nowhere
 * else:
 * In memory the timestamps are integers too (struct edc_session holds uint64_t), so a save
 * or a load never converts anything. Formatting happens only where a human reads it: the
 * CLI replies and the status JSON the TUI consumes.
 * Columns holding 0 mean "unknown" (the same sentinel the C structs use), never 1970. */
static const char *const EDC_SCHEMA_SQL =
    /* No `meta` key/value table: it was declared in v1 and never written to. A table that
     * nothing reads is a table that will quietly disagree with the real state. */

    "CREATE TABLE IF NOT EXISTS sessions ("
    /* AUTOINCREMENT for the same reason tasks.task_id uses it: a session id must never
     * be reused. A recycled id would land a new session on a removed one's directory and
     * replay its chat.jsonl. This is the invariant <ws>/.session_seq used to provide. */
    "  session_id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name                  TEXT NOT NULL,"
    "  created_at            INTEGER,"
    "  last_active           INTEGER,"
    "  workdir               TEXT,"
    "  active_task_id        INTEGER, /* tasks.task_id, 0/NULL = none */"
    "  foreground_group_id   INTEGER, /* groups.group_id, 0/NULL = none */"
    "  next_completion_seq   INTEGER NOT NULL DEFAULT 1,"
    /* Which session the UI is on. It used to live only in sessions.json's
     * active_session_id, which is why the session LIST also lived there: the index was the
     * only place the answer to "where am I" could be written. As a column it is one row to
     * update, it survives a kill, and the list can be a plain query.
     *
     * A partial unique index (below) enforces the invariant "at most one active session"
     * instead of trusting every writer to clear the others. */
    "  is_active             INTEGER NOT NULL DEFAULT 0,"
    /* `archived` and `broken` were forward-looking placeholders with no reader and no
     * writer (the load guard uses the in-memory state_quarantined flag). Declared columns
     * with no owner are how a schema rots. */
    "  memo_next_seq         INTEGER NOT NULL DEFAULT 0,"
    "  memo_next_entry_id    INTEGER NOT NULL DEFAULT 0,"
    "  memo_pruned_before    INTEGER NOT NULL DEFAULT 0,"
    "  updated_at            INTEGER NOT NULL"
    ");"
    /* At most one active session, enforced by the database rather than by every writer
     * remembering to clear the others. */
    "CREATE UNIQUE INDEX IF NOT EXISTS sessions_one_active ON sessions(is_active) WHERE is_active=1;"
    "CREATE INDEX IF NOT EXISTS sessions_by_created ON sessions(created_at);"

    "CREATE TABLE IF NOT EXISTS groups ("
    "  session_id         INTEGER NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,"
    /* No `leader_task_id`: the leader IS the task whose id equals `group_id`
     * (edc_task_group_new / set_leader both set group_id from the leader's task_id), so a
     * separate column could only ever hold the same string. Its `is_orphaned` twin stays:
     * a group whose leader left keeps its id but has no leader any more, and that is not
     * recoverable from the remaining rows. */
    "  group_id           INTEGER NOT NULL,"
    "  is_orphaned        INTEGER NOT NULL DEFAULT 0,"
    "  kind               TEXT NOT NULL DEFAULT 'user',"
    "  memory             TEXT,"
    "  fold_epoch         INTEGER NOT NULL DEFAULT 0,"
    "  memory_created_at  INTEGER NOT NULL DEFAULT 0,"
    "  memory_updated_at  INTEGER NOT NULL DEFAULT 0,"
    "  memory_covers_to   INTEGER NOT NULL DEFAULT 0,"
    "  ord                INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (session_id, group_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS groups_by_session ON groups(session_id, ord);"

    "CREATE TABLE IF NOT EXISTS group_fold_files ("
    "  session_id INTEGER NOT NULL,"
    "  group_id   INTEGER NOT NULL,"
    /* Which list a path belongs to. v7: without it the two lists had to share one flat
     * column, and the flush wrote NEITHER because it mistook the {"modified","read"} object
     * for a flat array -- the table was empty on every workspace since the store landed. */
    "  kind       TEXT NOT NULL,"
    "  ord        INTEGER NOT NULL,"  /* per (group, kind) */
    "  path       TEXT NOT NULL,"
    "  PRIMARY KEY (session_id, group_id, kind, ord),"
    "  FOREIGN KEY (session_id, group_id) REFERENCES groups(session_id, group_id) ON DELETE CASCADE"
    ");"

    "CREATE TABLE IF NOT EXISTS tasks ("
    /* AUTOINCREMENT, not a bare INTEGER PRIMARY KEY: SQLite then never hands out an id
     * twice, even after the row is deleted. That is the invariant the old .task_seq
     * high-water file existed to provide -- a task id must not be reused, or a resumed
     * task could collide with a new one. It costs one row in sqlite_sequence. */
    "  task_id                   INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session_id                INTEGER NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,"
    "  group_id                  INTEGER NOT NULL,"
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
    "  sink_group_id             INTEGER,"
    "  fold_cut_task             INTEGER,"
    "  fold_cut_turn             INTEGER NOT NULL DEFAULT 0,"
    "  fold_cut_prefix           INTEGER NOT NULL DEFAULT 0,"
    "  memo_cut_task             INTEGER,"
    "  memo_cut_turn             INTEGER NOT NULL DEFAULT 0,"
    "  memo_cut_prefix           INTEGER NOT NULL DEFAULT 0,"
    "  forked_from_task_id       INTEGER REFERENCES tasks(task_id),"
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
    /* v8: whether this task has received the memory CATALOGUE (the "what exists" card).
     * It is separate state, not `memo_seen_seq == 0`: one watermark cannot express both
     * "never saw the index" and "the feed is empty". */
    "  memo_baseline_done        INTEGER NOT NULL DEFAULT 0,"
    "  last_prefix_tokens        INTEGER NOT NULL DEFAULT 0,"
    "  redirect_stdout           TEXT,"
    "  redirect_stderr           TEXT,"
    "  redirect_append           INTEGER NOT NULL DEFAULT 0,"
    "  redirect_stderr_append    INTEGER NOT NULL DEFAULT 0,"
    "  todos                     TEXT,"
    "  todo_sync_gap             INTEGER NOT NULL DEFAULT 0,"
    "  updated_at                INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS tasks_by_group ON tasks(session_id, group_id, ord);"
    "CREATE INDEX IF NOT EXISTS tasks_by_fork  ON tasks(forked_from_task_id);"

    "CREATE TABLE IF NOT EXISTS task_deps ("
    "  target_task_id INTEGER NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,"
    "  source_task_id INTEGER NOT NULL,"
    "  type           TEXT NOT NULL,"
    "  completed      INTEGER NOT NULL DEFAULT 0,"
    "  cancelled      INTEGER NOT NULL DEFAULT 0,"
    "  injected       INTEGER NOT NULL DEFAULT 0,"
    "  ord            INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (target_task_id, source_task_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS task_deps_by_source ON task_deps(source_task_id);"

    "CREATE TABLE IF NOT EXISTS task_tool_calls ("
    "  task_id INTEGER NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,"
    "  ord     INTEGER NOT NULL,"
    "  call_id TEXT,"
    "  name    TEXT,"
    "  args    TEXT,"
    "  status  TEXT NOT NULL,"
    "  PRIMARY KEY (task_id, ord)"
    ");"

    "CREATE TABLE IF NOT EXISTS turns ("
    "  task_id       INTEGER NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,"
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
    "  id         TEXT NOT NULL PRIMARY KEY,"
    /* Denormalized on purpose: nearly every read asks for one session's or one group's
     * messages, and carrying the two keys here saves a join to tasks every time.
     *
     * These CAN drift -- they did: edc_msg_turn_move() re-keyed a turn without touching
     * them, so a moved message kept claiming its old group. Every statement that changes
     * task_id here has to re-derive them (see edc_msg_turn_move). They are correct by
     * discipline, not by construction, which is the price of the join they save. */
    "  session_id INTEGER NOT NULL,"
    "  group_id   INTEGER NOT NULL,"
    "  task_id    INTEGER NOT NULL,"
    "  turn_ord   INTEGER NOT NULL,"
    "  msg_ord    INTEGER NOT NULL,"
    /* Metadata the model never sees. Each of these used to live INSIDE the message JSON
     * next to the wire fields, which meant the same value existed twice (a `ts` column and
     * a "ts" string inside the JSON) and had to be converted back and forth. They are
     * columns now, and the JSON holds only what goes on the wire. */
    "  role       TEXT,"
    "  mid        TEXT,"
    "  ts         INTEGER NOT NULL DEFAULT 0,"
    "  render     TEXT,"
    "  synthetic  INTEGER NOT NULL DEFAULT 0,"
    "  payload    TEXT,"
    /* Token estimate, computed once when the message is appended. Token accounting is
     * then a SUM over an index range instead of re-estimating every message per request. */
    "  tokens     INTEGER NOT NULL DEFAULT 0,"
    /* THE WIRE BODY: the message minus the store-only fields above. Deliberately kept as
     * one JSON document rather than exploded into content/tool_calls/... columns: the
     * runtime passes a message through almost verbatim, and unknown provider fields
     * (reasoning_content, refusals, future additions) must ride along untouched. */
    "  body       TEXT NOT NULL,"
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
    /* One group's messages in storage order: what request assembly reads. session_id leads
     * so a session-wide scan (the recall/search paths) uses the same index. */
    "CREATE INDEX IF NOT EXISTS messages_by_group ON messages(session_id, group_id, task_id, turn_ord, msg_ord);"
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
    "  session_id  INTEGER NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,"
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
    "  session_id INTEGER NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,"
    "  seq        INTEGER NOT NULL,"
    "  ts         INTEGER NOT NULL DEFAULT 0,"
    "  entry_id   TEXT,"
    "  kind       TEXT,"
    "  title      TEXT,"
    "  summary    TEXT,"
    /* v8: EDC_MEMO_F_* mask of the fields this change touched. 0 = unknown (rows written
     * before v8). Persisted so a restart does not erase the layering of past changes. */
    "  changed    INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (session_id, seq)"
    ");";

/* The tables a healthy database must contain. Used by the post-DDL sanity check so a
 * half-applied schema fails here rather than as a mysterious SQL error later. */
static const char *const EDC_EXPECTED_TABLES[] = {
    "sessions", "groups", "group_fold_files", "tasks", "task_deps", "task_tool_calls",
    "turns", "messages", "memo_entries", "memo_events", NULL};

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
    switch ((uint64_t)sfs.f_type)
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
    /* Windows (NTFS/ReFS) and macOS (APFS/HFS+) are local by construction here; SQLite's
     * own locking is used on both. Nothing to check. */
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

/* ── One process per workspace ──────────────────────────────────────────────
 * WAL already does the locking that protects PAGES: readers and the writer never block
 * each other, and SQLite schedules concurrent writers itself. What WAL cannot protect is
 * MEANING. Two agent processes each hold a divergent in-memory tree and each write a
 * whole-session snapshot; SQLite commits both, the last one wins, and the user's data is
 * gone with no error anywhere. "One agent process per workspace" is therefore ours to
 * enforce.
 *
 * It is enforced with SQLite's OWN lock, not a second mechanism: locking_mode=EXCLUSIVE
 * says "this database belongs to this connection", and every other connection -- another
 * process, or a second one in this process -- gets SQLITE_BUSY until we close it. One
 * invariant, one lock.
 *
 * Deliberately NOT a flock:
 *  - not on the database file: on macOS/BSD flock() and the fcntl() locks SQLite uses are
 *    the same domain, so holding one there makes SQLite's own first read fail with
 *    SQLITE_BUSY and startup dies in quick_check;
 *  - not on a sidecar .lock file: the workspace is model-writable, so that is one more
 *    path the model can see, one more symlink to defend against, one more file for tests
 *    and users to clean up, and one more thing to get wrong -- for no gain, since SQLite
 *    already has the lock we need;
 *  - not on the workspace directory: same objection, one fewer file but still a second
 *    lock to keep in step with this one.
 *
 * The cost, stated plainly: while an agent runs, nothing else can read the database --
 * `sqlite3 edgeclaw.db` included. Anything that needs to look at the rows of a LIVE store
 * goes through edc_state_store_handle(). Diagnostics that only need the file (backup,
 * export, integrity check) run against a stopped agent. If a hot read ever becomes a hard
 * requirement, this is the decision to revisit, and the replacement is one flock on the
 * workspace directory -- still a single lock, still no new file. */

/* Take the database exclusively, and give "another instance has it" its own sentence.
 *
 * locking_mode=EXCLUSIVE is only a request: the lock is actually taken by the next
 * statement that reads the file. So the check has to force a read, and that read is the
 * same quick_check(1) startup wants anyway -- one statement, both answers. */
static bool claim_exclusive(sqlite3 *db, const char *path)
{
    if (!exec_sql(db, "PRAGMA locking_mode=EXCLUSIVE", "PRAGMA locking_mode"))
        return false;

    /* Verified, not assumed: a silently ignored pragma would mean no exclusion at all,
     * which is the same class of failure as a silently unhardened SQLite build. */
    char *mode = query_text(db, "PRAGMA locking_mode");
    const bool is_exclusive = mode && strcmp(mode, "exclusive") == 0;
    if (!is_exclusive)
        log_error("state_store: locking_mode is '%s', expected 'exclusive' -- refusing to "
                  "run without the one-process-per-workspace guarantee",
                  mode ? mode : "(none)");
    free(mode);
    if (!is_exclusive)
        return false;

    /* Cheap corruption probe before anything else touches the file, and the statement
     * that actually takes the lock. On failure the WAL is left in place for
     * `edgeclaw db salvage` rather than silently discarded. */
    char *integrity = query_text(db, "PRAGMA quick_check(1)");
    if (integrity && strcmp(integrity, "ok") == 0)
    {
        free(integrity);
        return true;
    }

    /* Distinguish "somebody else is here" from "this file is damaged": they need
     * different sentences and different fixes, and a bare SQLITE_BUSY is not a
     * diagnosable error. */
    const int rc = sqlite3_errcode(db);
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
        log_error("state_store: another edgeclaw instance is using this workspace (%s is "
                  "locked). Refusing to start -- two processes would overwrite each other's "
                  "session state.",
                  path);
    else
        log_error("state_store: database %s failed quick_check: %s (keep the -wal file for "
                  "salvage)",
                  path, integrity ? integrity : "(no answer)");
    free(integrity);
    return false;
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
 *
 *   locking_mode=EXCLUSIVE    "this database belongs to this connection": the one-process-
 *                             per-workspace guarantee, taken from SQLite instead of a
 *                             second lock of our own (see the Locking note above). Applied
 *                             FIRST in open(), before this set, because the statement that
 *                             takes the lock is also the corruption probe.
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
 * One schema version (`EDC_DB_VERSION`) and one upgrade direction: forward, one step at a
 * time, through the registry in edc_state_migrate.c.
 *
 * A mismatch used to be terminal -- "remove the file to recreate it". It is not any more:
 * from v1.0 on, a workspace that a previous EdgeClaw wrote is MIGRATED (backed up first,
 * verified, one transaction per step), and only a NEWER schema is refused, because reading
 * a newer file with an older binary is how data gets silently dropped. */
/* Every table the schema promises must exist. Shared by the create path and the migrate
 * path: a partial schema should fail here, with a sentence a user can act on, rather than
 * as a confusing SQL error in the middle of a flush. */
static bool require_expected_tables(sqlite3 *db, const char *when)
{
    for (int i = 0; EDC_EXPECTED_TABLES[i]; i++)
    {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s'",
                 EDC_EXPECTED_TABLES[i]);
        if (query_int(db, sql, 0) != 1)
        {
            log_error("state_store: %s left no '%s' table", when, EDC_EXPECTED_TABLES[i]);
            return false;
        }
    }
    return true;
}

/* Four cases, in the order they are decided:
 *
 *  1. same version        -> nothing to do.
 *  2. newer version       -> REFUSE. A database written by a newer build cannot be
 *                            understood by this one, and pretending otherwise would
 *                            corrupt it. The answer is "update EdgeClaw", never "delete".
 *  3. older, known        -> MIGRATE in place (edc_state_migrate.c): back up, prove the
 *                            backup opens, then one transaction per step.
 *  4. no version stamp    -> brand-new file: build the schema and stamp it.
 *
 * The third case is the one that matters to a user: before this existed, a schema bump
 * answered with "remove the file to recreate it", which is exactly how someone loses every
 * conversation they ever had. */
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

    if (version > EDC_DB_VERSION)
    {
        log_error("state_store: database schema is v%d, but this EdgeClaw (%s) understands "
                  "v%d -- a newer EdgeClaw wrote %s. Update EdgeClaw; this build will not "
                  "touch a schema it cannot read, because writing it back would lose "
                  "whatever the newer version added.",
                  version, edc_version(), EDC_DB_VERSION,
                  g_db_path ? g_db_path : "(the database)");
        return false;
    }

    if (version != 0)
    {
        char err[1024] = {0};
        if (!edc_migrate_run(db, g_db_path, version, err, sizeof err))
        {
            log_error("state_store: %s", err);
            return false;
        }
        if (!require_expected_tables(db, "migration"))
            return false;
        return true;
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

    if (!require_expected_tables(db, "schema creation"))
        return false;

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
        return ((uint64_t)st.st_ino == g_db_ino) &&
               ((uint64_t)st.st_dev == g_db_dev);
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
        const bool ok = refuse_symlink(wal, "write-ahead log") &&
                        refuse_symlink(shm, "shared memory");
        sdsfree(wal);
        sdsfree(shm);
        if (!ok)
            goto fail;
    }

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

    /* One process per workspace, then a corruption probe -- one statement each, see
     * claim_exclusive(). */
    if (!claim_exclusive(g_db, g_db_path))
        goto fail;

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
            g_db_dev = (uint64_t)st.st_dev;
            g_db_ino = (uint64_t)st.st_ino;
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

struct sqlite3 *edc_state_store_handle(void)
{
    return g_db;
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

/* Message JSON <-> columns. Defined next to the message read/write path further down; the
 * snapshot loader earlier in the file needs them too. */
static char *msg_body_text(const cJSON *msg);
static char *msg_payload_text(const cJSON *msg);
static cJSON *msg_from_row(sqlite3_stmt *st, int base);

/* The message columns, in the order msg_from_row() expects them. One list so the readers
 * cannot drift. Declared here because the snapshot loader below uses it too. */
#define MSG_SELECT_COLS "body,id,role,ts,render,synthetic,payload,mid"

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
    const struct edc_session *s;
    int64_t session_id;
};

/* Bind straight from the live struct. There is no intermediate document any more: the
 * previous version went struct -> cJSON (edc_state_serializer.c) -> sqlite3_bind_*, which
 * meant every field was written three times -- once in the struct, once in the serializer,
 * once here -- and a new field that missed one of the three disappeared silently. */
static void bind_session(sqlite3_stmt *st, void *ctx)
{
    struct session_bind *b = ctx;
    const struct edc_session *s = b->s;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)b->session_id);
    bind_str(st, 2, (s->name && *s->name) ? s->name : "");
    sqlite3_bind_int64(st, 3, (sqlite3_int64)s->created_at);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)s->last_active);
    bind_str(st, 5, s->workdir);
    /* INTEGER columns: a number bound as text is stored as text and reads back as 0 through
     * sqlite3_column_int64, silently losing the pointer. NULL when there is none, which is
     * what the tree used to leave behind. */
    if (s->active_task && s->active_task->task_id > 0)
        sqlite3_bind_int64(st, 6, (sqlite3_int64)s->active_task->task_id);
    else
        sqlite3_bind_null(st, 6);
    if (s->foreground_group && s->foreground_group->group_id > 0)
        sqlite3_bind_int64(st, 7, (sqlite3_int64)s->foreground_group->group_id);
    else
        sqlite3_bind_null(st, 7);
    bind_num(st, 8, (double)s->next_completion_seq);
    sqlite3_bind_int64(st, 9, (sqlite3_int64)time(NULL));
}

static const char *INSERT_GROUP =
    "INSERT INTO groups(session_id,group_id,is_orphaned,kind,memory,fold_epoch,"
    "memory_created_at,memory_updated_at,memory_covers_to,ord) "
    "VALUES(?,?,?,?,?,?,?,?,?,?) "
    "ON CONFLICT(session_id,group_id) DO UPDATE SET is_orphaned=excluded.is_orphaned,"
    "kind=excluded.kind,memory=excluded.memory,"
    "fold_epoch=excluded.fold_epoch,memory_created_at=excluded.memory_created_at,"
    "memory_updated_at=excluded.memory_updated_at,memory_covers_to=excluded.memory_covers_to,"
    "ord=excluded.ord";

struct group_bind
{
    const struct edc_task_group *g;
    int64_t session_id;
    int ord;
};

static void bind_group(sqlite3_stmt *st, void *ctx)
{
    struct group_bind *b = ctx;
    const struct edc_task_group *g = b->g;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)b->session_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)g->group_id);
    sqlite3_bind_int(st, 3, g->is_orphaned ? 1 : 0);
    bind_str(st, 4, edc_task_group_kind_name(g->kind));
    bind_str(st, 5, g->memory);
    bind_num(st, 6, (double)g->fold_epoch);
    bind_num(st, 7, (double)g->memory_created_at);
    bind_num(st, 8, (double)g->memory_updated_at);
    bind_num(st, 9, (double)g->memory_covers_to);
    sqlite3_bind_int(st, 10, b->ord);
}

static const char *INSERT_TASK =
    "INSERT INTO tasks(task_id,session_id,group_id,ord,description,raw_input,step,life,end_state,"
    "blocked_by,blocked_end,fail,is_group_leader,bg,tool_only,deferred_vars,memo_kind,sink,"
    "sink_group_id,fold_cut_task,fold_cut_turn,fold_cut_prefix,memo_cut_task,memo_cut_turn,"
    "memo_cut_prefix,forked_from_task_id,fork_point_completion_seq,output,completion_seq,"
    "tokens_prompt,tokens_completion,tokens_total,tokens_cached,last_total_tokens,last_fold_epoch,"
    "last_hist_len,last_turn_len,memo_seen_seq,memo_baseline_done,last_prefix_tokens,"
    "redirect_stdout,redirect_stderr,"
    "redirect_append,redirect_stderr_append,todos,todo_sync_gap,updated_at) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
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
    "memo_baseline_done=excluded.memo_baseline_done,"
    "last_prefix_tokens=excluded.last_prefix_tokens,redirect_stdout=excluded.redirect_stdout,"
    "redirect_stderr=excluded.redirect_stderr,redirect_append=excluded.redirect_append,"
    "redirect_stderr_append=excluded.redirect_stderr_append,todos=excluded.todos,"
    "todo_sync_gap=excluded.todo_sync_gap,updated_at=excluded.updated_at";

struct task_bind
{
    const struct edc_task *t;
    int64_t session_id;
    int ord;
};

static void bind_task(sqlite3_stmt *st, void *ctx)
{
    struct task_bind *b = ctx;
    const struct edc_task *t = b->t;
    int i = 1;
    sqlite3_bind_int64(st, i++, (sqlite3_int64)t->task_id);
    sqlite3_bind_int64(st, i++, (sqlite3_int64)b->session_id);
    sqlite3_bind_int64(st, i++, (sqlite3_int64)t->group_id);
    sqlite3_bind_int(st, i++, b->ord);
    bind_str(st, i++, t->description ? t->description : "");
    bind_str(st, i++, t->raw_input);
    bind_str(st, i++, edc_task_step_name(t->step));
    bind_str(st, i++, edc_task_life_name(t->life));
    bind_str(st, i++, edc_task_end_name(t->end));
    bind_str(st, i++, t->blocked_by);
    /* Only meaningful when end == BLOCKED; NULL otherwise, exactly where the tree used to
     * omit the key. The reader looks at this column only on that branch anyway. */
    if (t->end == EDC_END_BLOCKED)
        bind_str(st, i++, edc_task_end_name(t->blocked_end));
    else
        sqlite3_bind_null(st, i++);
    /* NULL, not the string "NONE": every row already on disk has NULL for "no failure",
     * and a column holding both spellings is a column whose "IS NULL" query lies. */
    if (t->fail != EDC_FAIL_NONE)
        bind_str(st, i++, edc_task_fail_name(t->fail));
    else
        sqlite3_bind_null(st, i++);
    sqlite3_bind_int(st, i++, t->is_group_leader ? 1 : 0);
    sqlite3_bind_int(st, i++, t->is_background ? 1 : 0);
    sqlite3_bind_int(st, i++, t->tool_only ? 1 : 0);
    sqlite3_bind_int(st, i++, t->has_deferred_vars ? 1 : 0);
    /* The column's default is 'curate' (lowercase) and every existing row spells it that
     * way, while edc_task_memo_kind_name() answers "CURATE" because it is also a display
     * spelling. Classify accepts either, so this is about not splitting the vocabulary in
     * one column -- "WHERE memo_kind='curate'" has to keep meaning what it says. */
    bind_str(st, i++, t->memo_kind == EDC_MEMO_KIND_CURATE ? "curate"
                                                            : edc_task_memo_kind_name(t->memo_kind));
    bind_str(st, i++, edc_task_sink_name(t->sink));
    if (t->sink_group && t->sink_group->group_id > 0)
        sqlite3_bind_int64(st, i++, (sqlite3_int64)t->sink_group->group_id);
    else
        sqlite3_bind_null(st, i++);
    /* 0 = no cut; NULL in the column, so the reader treats both the same. */
    if (t->fold_cut_task_id > 0)
        sqlite3_bind_int64(st, i++, (sqlite3_int64)t->fold_cut_task_id);
    else
        sqlite3_bind_null(st, i++);
    bind_num(st, i++, (double)t->fold_cut_turn);
    bind_num(st, i++, (double)t->fold_cut_prefix);
    if (t->memo_cut_task_id > 0)
        sqlite3_bind_int64(st, i++, (sqlite3_int64)t->memo_cut_task_id);
    else
        sqlite3_bind_null(st, i++);
    bind_num(st, i++, (double)t->memo_cut_turn);
    bind_num(st, i++, (double)t->memo_cut_prefix);
    /* 0 = not a fork; a 0 FK would point at no task, so the column stays NULL. */
    if (t->forked_from && t->forked_from->task_id > 0)
        sqlite3_bind_int64(st, i++, (sqlite3_int64)t->forked_from->task_id);
    else
        sqlite3_bind_null(st, i++);
    bind_num(st, i++, (double)t->fork_point_completion_seq);
    bind_str(st, i++, t->output);
    bind_num(st, i++, (double)t->completion_seq);
    bind_num(st, i++, (double)t->tokens_prompt);
    bind_num(st, i++, (double)t->tokens_completion);
    bind_num(st, i++, (double)t->tokens_total);
    bind_num(st, i++, (double)t->tokens_cached);
    bind_num(st, i++, (double)t->last_total_tokens);
    bind_num(st, i++, (double)t->last_fold_epoch);
    bind_num(st, i++, (double)t->last_hist_len);
    bind_num(st, i++, (double)t->last_turn_len);
    bind_num(st, i++, (double)t->memo_seen_seq);
    sqlite3_bind_int(st, i++, t->memo_baseline_done ? 1 : 0);
    bind_num(st, i++, (double)t->last_prefix_tokens);
    bind_str(st, i++, t->redirect_stdout);
    bind_str(st, i++, t->redirect_stderr);
    sqlite3_bind_int(st, i++, t->redirect_append ? 1 : 0);
    sqlite3_bind_int(st, i++, t->redirect_stderr_append ? 1 : 0);
    {
        /* todos is an opaque JSON document: store its text, not a re-encode of it. */
        if (t->todos)
        {
            char *txt = cJSON_PrintUnformatted(t->todos);
            bind_str(st, i++, txt);
            cJSON_free(txt);
        }
        else
            sqlite3_bind_null(st, i++);
    }
    bind_num(st, i++, (double)t->todo_sync_gap);
    sqlite3_bind_int64(st, i, (sqlite3_int64)time(NULL));
}

static const char *INSERT_DEP =
    "INSERT OR REPLACE INTO task_deps(target_task_id,source_task_id,type,completed,cancelled,"
    "injected,ord) VALUES(?,?,?,?,?,?,?)";

struct dep_bind
{
    int64_t target;
    const struct edc_task_dep *d;
    int ord;
};

static void bind_dep(sqlite3_stmt *st, void *ctx)
{
    struct dep_bind *b = ctx;
    const struct edc_task_dep *d = b->d;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)b->target);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)(d->source ? d->source->task_id : 0));
    bind_str(st, 3, edc_task_dep_type_name(d->type));
    sqlite3_bind_int(st, 4, d->completed ? 1 : 0);
    sqlite3_bind_int(st, 5, d->cancelled ? 1 : 0);
    sqlite3_bind_int(st, 6, d->injected ? 1 : 0);
    sqlite3_bind_int(st, 7, b->ord);
}

static const char *INSERT_TOOL_CALL =
    "INSERT OR REPLACE INTO task_tool_calls(task_id,ord,call_id,name,args,status) VALUES(?,?,?,?,?,?)";

struct tool_call_bind
{
    int64_t task_id;
    const struct edc_tool_call_record *rec;
    int ord;
};

static void bind_tool_call(sqlite3_stmt *st, void *ctx)
{
    struct tool_call_bind *b = ctx;
    const struct edc_tool_call_record *r = b->rec;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)b->task_id);
    sqlite3_bind_int(st, 2, b->ord);
    bind_str(st, 3, r->call_id);
    bind_str(st, 4, r->name);
    if (r->args)
    {
        char *txt = cJSON_PrintUnformatted(r->args);
        bind_str(st, 5, txt);
        cJSON_free(txt);
    }
    else
        sqlite3_bind_null(st, 5);
    bind_str(st, 6, r->status == TOOL_CALL_EXECUTING ? "executing" : "pending");
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
    int64_t task_id;
    int turn_ord;
    const struct edc_turn *turn;
    int is_active;
};

static void bind_turn(sqlite3_stmt *st, void *ctx)
{
    struct turn_bind *b = ctx;
    const struct edc_turn *turn = b->turn;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)b->task_id);
    sqlite3_bind_int(st, 2, b->turn_ord);
    bind_num(st, 3, (double)turn->folded_prefix);
    bind_num(st, 4, (double)turn->memo_prefix);
    sqlite3_bind_int(st, 5, b->is_active);

    /* Counts the runtime reads back. On the live path edc_msg_append keeps them current;
     * INSERT_TURN recomputes both from the rows anyway (see its comment), so these two
     * bindings are a formality -- but they are the shape of the insert. */
    sqlite3_bind_int(st, 6, turn->msg_count);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)edc_turn_tokens_from((struct edc_turn *)turn, 0));
}

/* ── flush bookkeeping ──────────────────────────────────────────────────────
 * Small helpers for the "upsert, then remove what disappeared" shape of the flush. */

static bool mark_seen_group(int64_t group_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO flush_seen_group(group_id) VALUES(?)", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)group_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool mark_seen_task(int64_t task_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO flush_seen_task(task_id) VALUES(?)", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* The memory's two tables need the same treatment as tasks and groups: the session memory
 * removes rows in RAM only -- `edc_session_memory_delete` drops an entry for real and
 * `edc_session_memory_trim_events` drops old events -- so without a seen-set the next load
 * would resurrect them. (That was a real bug: the flush only ever inserted.) */
static bool mark_seen_memo_entry(const char *entry_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO flush_seen_memo_entry(entry_id) VALUES(?)", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    bind_str(st, 1, entry_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool mark_seen_memo_event(uint64_t seq)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO flush_seen_memo_event(seq) VALUES(?)", -1, &st, NULL) !=
        SQLITE_OK)
        return false;
    bind_num(st, 1, (double)seq);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Write one tracked-file list. |at| is the module's read-only accessor, so the caller does
 * not need to know how many entries there are beyond the count it passes. */
static bool bind_fold_files(sqlite3 *db, int64_t session_id, int64_t group_id, const char *kind,
                            const struct edc_fold_files *files, size_t count,
                            const char *(*at)(const struct edc_fold_files *, size_t))
{
    if (!count)
        return true; /* an empty list writes nothing; the delete already cleared the group */

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT OR REPLACE INTO group_fold_files(session_id,group_id,kind,ord,path) "
                           "VALUES(?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK)
        return false;

    bool ok = true;
    for (size_t i = 0; i < count; i++)
    {
        const char *path = at(files, i);
        if (!path)
            continue;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)group_id);
        bind_str(st, 3, kind);
        sqlite3_bind_int(st, 4, (int)i);
        bind_str(st, 5, path);
        if (sqlite3_step(st) != SQLITE_DONE)
            ok = false;
        sqlite3_reset(st);
        if (!ok)
            break;
    }
    sqlite3_finalize(st);
    return ok;
}

static bool delete_group_fold_files(int64_t session_id, int64_t group_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM group_fold_files WHERE session_id=? AND group_id=?", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)group_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Turns at or beyond |keep| no longer exist for this task: drop them. Their messages go
 * with them through the cascade, which is correct -- a turn that was removed has no
 * conversation left to keep. */
static bool delete_turns_from(int64_t task_id, int keep)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM turns WHERE task_id=? AND turn_ord>=?", -1, &st, NULL) !=
        SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
    sqlite3_bind_int(st, 2, keep);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Everything that was in the database but is not in this snapshot. Runs LAST, inside the
 * transaction, and only inside the session being flushed. `memo_flushed` gates the memory
 * tables: without a memory in hand their seen-sets are empty, and "not seen" would then mean
 * "delete everything" instead of "nothing was checked". */
static bool delete_stale(int64_t session_id, bool memo_flushed)
{
    /* Tasks and groups removed from the session. Cascades clean their turns/messages. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "DELETE FROM tasks WHERE session_id=? AND task_id NOT IN "
                           "(SELECT task_id FROM flush_seen_task)",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
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
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok || !memo_flushed)
        return ok;

    /* The memory's two tables: an entry deleted for real and an event trimmed away leave the
     * RAM tables but, without this, stayed in the database and came back on the next load. */
    st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "DELETE FROM memo_entries WHERE session_id=? AND entry_id NOT IN "
                           "(SELECT entry_id FROM flush_seen_memo_entry)",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        return false;

    st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "DELETE FROM memo_events WHERE session_id=? AND seq NOT IN "
                           "(SELECT seq FROM flush_seen_memo_event)",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Write one turn (its row plus its messages) for |task_id|. */
static bool flush_turn(sqlite3 *db, int64_t task_id, int turn_ord, const struct edc_turn *turn,
                      int is_active)
{
    struct turn_bind tb = {task_id, turn_ord, turn, is_active};
    /* No message loop any more: a turn's messages are already rows (edc_msg_append), and the
     * snapshot never carried them. The loop that used to be here read a "messages" key that
     * nothing ever wrote. */
    return run_stmt(db, INSERT_TURN, "turn", bind_turn, &tb);
}

/* Write one task and everything hanging off it. */
static bool flush_task(sqlite3 *db, int64_t session_id, const struct edc_task *t, int ord)
{
    if (!t || t->task_id <= 0)
        return true; /* nothing to key on: skip rather than fail the whole flush */

    struct task_bind tb = {t, session_id, ord};
    if (!run_stmt(db, INSERT_TASK, "task", bind_task, &tb))
        return false;
    if (!mark_seen_task(t->task_id))
        return false;

    /* deps (outgoing edges) */
    {
        int i = 0;
        for (const struct edc_task_dep *d = t->deps; d; d = d->next)
        {
            struct dep_bind db_ind = {t->task_id, d, i++};
            if (!run_stmt(db, INSERT_DEP, "dep", bind_dep, &db_ind))
                return false;
        }
    }

    /* the incomplete-tool-call ledger */
    {
        int i = 0;
        for (const struct edc_tool_call_record *c = t->tool_calls; c; c = c->next)
        {
            struct tool_call_bind cb = {t->task_id, c, i++};
            if (!run_stmt(db, INSERT_TOOL_CALL, "tool_call", bind_tool_call, &cb))
                return false;
        }
    }

    /* committed turns, then the in-flight one at the tail (so (task_id, turn_ord) stays
     * unique). */
    int turn_ord = 0;
    for (size_t qi = t->turns.head; qi < t->turns.tail; qi++)
    {
        if (!flush_turn(db, t->task_id, turn_ord, t->turns.data[qi], 0))
            return false;
        turn_ord++;
    }
    if (t->active_turn)
    {
        if (!flush_turn(db, t->task_id, turn_ord, t->active_turn, 1))
            return false;
        turn_ord++;
    }

    /* Turns past the end no longer exist (a revert shortened the conversation). Dropping
     * them takes their message rows with them, which is the intent. */
    return delete_turns_from(t->task_id, turn_ord);
}

static bool flush_memo(sqlite3 *db, int64_t session_id, const struct edc_session_memory *memo)
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
        sqlite3_bind_int64(st, 1, (sqlite3_int64)memo->next_seq);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)memo->next_entry_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)memo->pruned_before);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)session_id);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok)
            return false;
    }

    for (size_t i = 0; i < memo->event_count; i++)
    {
        const struct edc_memo_event *e = &memo->events[i];
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                               "INSERT OR REPLACE INTO memo_events(session_id,seq,ts,entry_id,"
                               "kind,title,summary,changed) VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
        bind_num(st, 2, (double)e->seq);
        bind_num(st, 3, (double)e->ts);
        bind_str(st, 4, e->entry_id);
        bind_str(st, 5, edc_memo_event_kind_name(e->kind));
        bind_str(st, 6, e->title);
        bind_str(st, 7, e->summary);
        sqlite3_bind_int64(st, 8, (sqlite3_int64)e->changed);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok)
            return false;
        if (!mark_seen_memo_event(e->seq))
            return false;
    }

    for (size_t i = 0; i < memo->entry_count; i++)
    {
        const struct edc_memo_entry *e = &memo->entries[i];
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                               "INSERT OR REPLACE INTO memo_entries(session_id,entry_id,title,"
                               "summary,detail,sources,proposed_by,created_seq,updated_seq,"
                               "created_at,updated_at,superseded) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                               -1, &st, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
        bind_str(st, 2, e->id);
        bind_str(st, 3, e->title);
        bind_str(st, 4, e->summary);
        bind_str(st, 5, e->detail);
        bind_str(st, 6, e->sources);
        bind_str(st, 7, e->proposed_by);
        bind_num(st, 8, (double)e->created_seq);
        bind_num(st, 9, (double)e->updated_seq);
        bind_num(st, 10, (double)e->created_at);
        bind_num(st, 11, (double)e->updated_at);
        sqlite3_bind_int(st, 12, e->superseded ? 1 : 0);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok)
            return false;
        if (!mark_seen_memo_entry(e->id))
            return false;
    }
    return true;
}

/* Pages the pager has written since the connection opened (WAL mode: pages appended to
 * the -wal). Cumulative; the caller subtracts. Returns -1 when not measurable. */
static int64_t pages_written_now(int64_t *page_size_out)
{
    int cur = 0, hiwtr = 0;
    if (sqlite3_db_status(g_db, SQLITE_DBSTATUS_CACHE_WRITE, &cur, &hiwtr, 0) != SQLITE_OK)
        return -1;
    sqlite3_stmt *st = NULL;
    int64_t page_size = 0;
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
    return (int64_t)cur;
}

bool edc_state_store_flush_measured(struct edc_session *session, struct edc_scheduler *sched,
                                    struct edc_state_flush_cost *out)
{
    if (out)
    {
        out->io_bytes = -1;
        out->rows = -1;
    }

    if (!g_db || !session || session->session_id <= 0)
        return false;

    /* Sampled before BEGIN: the page counter and the change counter must bracket the whole
     * transaction, including the serialize that this function does first. */
    int64_t page_size = 0;
    const int64_t pages_before = out ? pages_written_now(&page_size) : -1;
    const int rows_before = out ? sqlite3_total_changes(g_db) : 0;

    const int64_t session_id = session->session_id;
    bool ok = false;

    if (!exec_sql(g_db, "BEGIN IMMEDIATE", "flush BEGIN"))
        return false;

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
                      "CREATE TEMP TABLE IF NOT EXISTS flush_seen_group(group_id INTEGER PRIMARY KEY);"
                      "CREATE TEMP TABLE IF NOT EXISTS flush_seen_task(task_id INTEGER PRIMARY KEY);"
                      "CREATE TEMP TABLE IF NOT EXISTS flush_seen_memo_entry(entry_id TEXT PRIMARY KEY);"
                      "CREATE TEMP TABLE IF NOT EXISTS flush_seen_memo_event(seq INTEGER PRIMARY KEY);"
                      "DELETE FROM flush_seen_group;"
                      "DELETE FROM flush_seen_task;"
                      "DELETE FROM flush_seen_memo_entry;"
                      "DELETE FROM flush_seen_memo_event;",
                      "flush seen-sets"))
            goto rollback;
    }

    {
        struct session_bind sb = {session, session_id};
        if (!run_stmt(g_db, INSERT_SESSION, "session row", bind_session, &sb))
            goto rollback;
    }

    /* Walk the tree itself. The order is the session's own member order, which is the order
     * the list was built in, so a reload reproduces the same sequence. */
    {
        struct edc_task_group *g;
        int ord = 0;
        DL_FOREACH(session->groups, g)
        {
            struct group_bind gb = {g, session_id, ord++};
            if (!run_stmt(g_db, INSERT_GROUP, "group", bind_group, &gb))
                goto rollback;
            if (!mark_seen_group(g->group_id))
                goto rollback;
            /* The tracked-file rows are replaced wholesale: a path dropped from the group
             * must not survive as a stale row. They are not an ancestor of anything. */
            if (!delete_group_fold_files(session_id, g->group_id))
                goto rollback;

            /* Two lists, two kinds, each numbered from 0 -- the order is the list order.
             * Written through edc_fold_files_*() so this file does not carry a second copy of
             * how the lists are shaped; the first version read them as one flat array, which
             * is why this table was empty for its whole life. */
            if (g->fold_files &&
                !bind_fold_files(g_db, session_id, g->group_id, "modified", g->fold_files,
                                 edc_fold_files_modified_count(g->fold_files),
                                 edc_fold_files_modified_at))
                goto rollback;
            if (g->fold_files &&
                !bind_fold_files(g_db, session_id, g->group_id, "read", g->fold_files,
                                 edc_fold_files_read_count(g->fold_files),
                                 edc_fold_files_read_at))
                goto rollback;
        }
    }

    {
        /* ONE counter for the whole session, not one per group: `tasks.ord` is read back as
         * "ORDER BY group_id, ord", so a per-group counter would renumber every group after
         * the first and rewrite rows that did not change. */
        struct edc_task_group *g;
        int ord = 0;
        DL_FOREACH(session->groups, g)
        {
            struct edc_task *t;
            LL_FOREACH2(g->members, t, group_next)
            {
                if (!flush_task(g_db, session_id, t, ord++))
                    goto rollback;
            }
        }
    }

    const bool memo_flushed = (session->memo != NULL);
    if (!flush_memo(g_db, session_id, session->memo))
        goto rollback;

    if (!delete_stale(session_id, memo_flushed))
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
        const int64_t pages_after = pages_written_now(NULL);
        out->io_bytes = (pages_before >= 0 && pages_after >= 0)
                            ? (pages_after - pages_before) * page_size
                            : -1;
        out->rows = (int64_t)(sqlite3_total_changes(g_db) - rows_before);
    }
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

static const char *col_str(sqlite3_stmt *st, int i)
{
    return sqlite3_column_type(st, i) == SQLITE_NULL ? NULL : (const char *)sqlite3_column_text(st, i);
}

/* ── one task, with its child collections ─────────────────────────────────── */

struct load_task_ctx
{
    cJSON *task;
};

/* The task columns, in the order `edc_state_store_load` reads them by index. Kept in one
 * place so the SELECT and the index arithmetic in load() cannot drift silently. */
static const char *SELECT_TASK_COLS =
    "task_id,group_id,session_id,description,raw_input,step,life,end_state,blocked_by,"
    "blocked_end,fail,is_group_leader,bg,tool_only,deferred_vars,memo_kind,sink,sink_group_id,"
    "fold_cut_task,fold_cut_turn,fold_cut_prefix,memo_cut_task,memo_cut_turn,memo_cut_prefix,"
    "forked_from_task_id,fork_point_completion_seq,output,completion_seq,tokens_prompt,"
    "tokens_completion,tokens_total,tokens_cached,last_total_tokens,last_fold_epoch,last_hist_len,"
    "last_turn_len,memo_seen_seq,memo_baseline_done,last_prefix_tokens,redirect_stdout,redirect_stderr,redirect_append,"
    "redirect_stderr_append,todos,todo_sync_gap";

static bool session_exists(int64_t session_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT 1 FROM sessions WHERE session_id=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    const bool found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  load: rows -> live structs, directly
 *
 *  This used to assemble a cJSON document from the columns and hand it to
 *  edc_state_deserialize(), which mapped every field a second time (columns -> JSON ->
 *  struct) and kept the restart rules somewhere other than the store that owns the data.
 *  What the document was really carrying is the ORDER of the steps below and their
 *  normalization; both are kept, the document is not.
 * ══════════════════════════════════════════════════════════════════════════ */

struct load_task_slot
{
    int64_t id;
    struct edc_task *task;
    struct load_task_slot *next;
};

struct load_group_slot
{
    int64_t id;
    struct edc_task_group *group;
    struct load_group_slot *next;
};

#define LOAD_MAP_BUCKETS 128

static unsigned load_map_bucket(int64_t id)
{
    uint64_t x = (uint64_t)id;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (unsigned)(x % LOAD_MAP_BUCKETS);
}

static void load_task_put(struct load_task_slot **map, int64_t id, struct edc_task *task)
{
    unsigned b = load_map_bucket(id);
    struct load_task_slot *e = calloc(1, sizeof(*e));
    e->id = id;
    e->task = task;
    e->next = map[b];
    map[b] = e;
}

static struct edc_task *load_task_get(struct load_task_slot **map, int64_t id)
{
    if (id <= 0)
        return NULL;
    for (struct load_task_slot *e = map[load_map_bucket(id)]; e; e = e->next)
        if (e->id == id)
            return e->task;
    return NULL;
}

static void load_task_map_free(struct load_task_slot **map)
{
    for (int i = 0; i < LOAD_MAP_BUCKETS; i++)
    {
        struct load_task_slot *e = map[i];
        while (e)
        {
            struct load_task_slot *next = e->next;
            free(e);
            e = next;
        }
    }
}

static void load_group_put(struct load_group_slot **map, int64_t id, struct edc_task_group *group)
{
    unsigned b = load_map_bucket(id);
    struct load_group_slot *e = calloc(1, sizeof(*e));
    e->id = id;
    e->group = group;
    e->next = map[b];
    map[b] = e;
}

static struct edc_task_group *load_group_get(struct load_group_slot **map, int64_t id)
{
    if (id <= 0)
        return NULL;
    for (struct load_group_slot *e = map[load_map_bucket(id)]; e; e = e->next)
        if (e->id == id)
            return e->group;
    return NULL;
}

static void load_group_map_free(struct load_group_slot **map)
{
    for (int i = 0; i < LOAD_MAP_BUCKETS; i++)
    {
        struct load_group_slot *e = map[i];
        while (e)
        {
            struct load_group_slot *next = e->next;
            free(e);
            e = next;
        }
    }
}

static int64_t col_i64(sqlite3_stmt *st, int i)
{
    return sqlite3_column_type(st, i) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, i);
}

/* Detach |task| from every group's member list, so a task that two groups both claim ends up
 * in exactly one. The member chain runs on `group_next` (not `next`) -- scanning with the
 * wrong field silently does nothing, which is how the same object once ended up on two lists
 * and got freed twice. */
static void load_detach_from_groups(struct edc_session *session, struct edc_task *task, int64_t task_id,
                                   struct edc_task_group *only_group)
{
    if (!session || !task)
        return;
    struct edc_task_group *g;
    DL_FOREACH(session->groups, g)
    {
        if (only_group && g != only_group)
            continue;
        struct edc_task *t;
        LL_FOREACH2(g->members, t, group_next)
        {
            if (t != task)
                continue;
            LL_DELETE2(g->members, t, group_next);
            if (g->member_count > 0)
                g->member_count--;
            log_warn("load: task %" PRId64 " detached from group %" PRId64 " (a task belongs to one group only)",
                     (int64_t)task_id, (int64_t)g->group_id);
            break;
        }
    }
}

/* Final reconciliation of the dependency graph.
 *
 * The runtime fires cancel/complete events in time order; rows have no such order, so
 * replaying them one by one would leave states the running process never had. This converges
 * once, and it also mirrors a cascade the rows may not have recorded (a source that ended
 * without producing takes its dependent with it -- the source's end can be newer than the
 * last save). BARRIER and UNLESS both RELEASE instead: an order satisfied, or a source that
 * did not succeed, is exactly why those two exist. */
static void load_reconcile_deps(struct edc_task **tasks, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        struct edc_task *task = tasks[i];
        if (!task)
            continue;

        struct edc_task_dep *dep;
        LL_FOREACH(task->deps, dep)
        {
            if (dep->cancelled || dep->completed || !dep->source)
                continue;
            if (edc_task_is_finished(dep->source) && !edc_task_is_completed(dep->source))
            {
                if (edc_task_is_cancelled(dep->source))
                    edc_task_on_dep_cancelled(task, dep->source);
                else
                    /* ERROR and INTERRUPTED: the loop did not finish the source, and an
                     * interrupt is a manufactured error -- both cascade as a failure. */
                    edc_task_on_dep_failed(task, dep->source);
            }
            else if (edc_task_is_completed(dep->source) && dep->type == TASK_DEP_UNLESS)
            {
                /* The inverse rule needs the same mirror: the source DID produce, so a `||`
                 * dependent must be skipped even though the rows still say the edge is out. */
                edc_task_on_dep_completed(task, dep->source);
            }
        }

        size_t total = 0, completed = 0;
        LL_FOREACH(task->deps, dep)
        {
            if (dep->cancelled)
                continue;
            total++;
            if (dep->completed)
                completed++;
        }
        task->deps_total = total;
        task->deps_completed = completed;
        if (edc_task_awaits_deps(task) && completed >= total)
            edc_task_mark_ready(task);
    }
}

bool edc_state_store_load(struct edc_session *session, struct edc_scheduler *sched)
{
    if (!g_db || !session || session->session_id <= 0)
        return false;

    const int64_t session_id = session->session_id;
    if (!session_exists(session_id))
        return false; /* a brand-new session: nothing on disk, not a failure */

    /* The loader BUILDS the tree; it does not merge into whatever the session carried. */
    session->groups = NULL;
    session->active_task = NULL;
    session->foreground_group = NULL;

    struct load_task_slot *task_map[LOAD_MAP_BUCKETS] = {0};
    struct load_group_slot *group_map[LOAD_MAP_BUCKETS] = {0};
    struct edc_task **tasks = NULL;
    size_t task_count = 0, task_cap = 0;
    int64_t active_task_id = 0;
    int64_t foreground_group_id = 0;
    bool ok = false;

    /* ── 1. the session row ─────────────────────────────────────────────── */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "SELECT name,created_at,last_active,workdir,active_task_id,"
                               "foreground_group_id,next_completion_seq,memo_next_seq,"
                               "memo_next_entry_id,memo_pruned_before FROM sessions WHERE session_id=?",
                               -1, &st, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
        if (sqlite3_step(st) != SQLITE_ROW)
        {
            sqlite3_finalize(st);
            goto done;
        }
        const char *name = col_str(st, 0);
        if (name && *name)
        {
            if (session->name)
                sdsfree(session->name);
            session->name = sdsnew(name);
        }
        session->created_at = (uint64_t)col_i64(st, 1);
        session->last_active = (uint64_t)col_i64(st, 2);
        active_task_id = col_i64(st, 4);
        foreground_group_id = col_i64(st, 5);
        session->next_completion_seq = (uint32_t)col_i64(st, 6);

        /* Through the API, so the sandbox grant happens with it. A path that no longer
         * resolves leaves the session unbound rather than failing the whole load. */
        const char *workdir = col_str(st, 3);
        if (workdir && *workdir)
        {
            EdcError *werr = NULL;
            if (!edc_session_set_workdir(session, workdir, &werr))
            {
                log_warn("load: session %s cannot restore working directory '%s': %s",
                         edc_id_str(session_id), workdir, werr ? werr->message : "unknown");
                if (werr)
                    edc_error_free(werr);
            }
        }

        /* Session memory: a fresh table carrying the stored counters. */
        edc_session_memory_free(session->memo);
        session->memo = edc_session_memory_new();
        if (!session->memo)
        {
            sqlite3_finalize(st);
            goto done;
        }
        session->memo->next_seq = (uint64_t)col_i64(st, 7);
        session->memo->next_entry_id = (uint64_t)col_i64(st, 8);
        session->memo->pruned_before = (uint64_t)col_i64(st, 9);
        sqlite3_finalize(st);
    }

    /* ── 2. tasks ────────────────────────────────────────────────────────
     *
     * The restart rules, and why they are exactly these:
     *
     *   - `step` is kept VERBATIM. It says where the task was, and resume has to know.
     *   - `life` converges: nothing runs after a restart, so ALIVE becomes INTERRUPTED --
     *     except a fold/curation task, which goes back to READY so the end-of-message scan
     *     redispatch it (it keeps its active_turn, sink group and cut).
     *   - `end` NONE is derived from `life`: a CANCELLED task is treated as the user's stop
     *     and an ERROR one as its own failure, never a cascade -- auto-releasing on a guess
     *     is what this axis exists to prevent.
     */
    {
        char sql[1024];
        snprintf(sql, sizeof(sql), "SELECT %s FROM tasks WHERE session_id=? ORDER BY task_id",
                 SELECT_TASK_COLS);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            const int64_t task_id = col_i64(st, 0);
            if (task_id <= 0)
                continue;

            const char *desc = col_str(st, 3);
            struct edc_task *task = edc_task_new(task_id, session_id, col_i64(st, 1), desc ? desc : "",
                                                 edc_channel_adapter);
            if (!task)
                continue;

            const char *raw_input = col_str(st, 4);
            if (raw_input && *raw_input)
                task->raw_input = sdsnew(raw_input);

            task->step = edc_task_classify_step(col_str(st, 5));
            task->life = edc_task_classify_life(col_str(st, 6));
            task->sink = edc_task_classify_sink(col_str(st, 16));
            if (task->life == EDC_LIFE_ALIVE)
            {
                if (edc_task_is_memory_sink(task))
                    task->step = EDC_TASKSTEP_READY;
                else
                    task->life = EDC_LIFE_INTERRUPTED;
            }

            task->end = edc_task_classify_end(col_str(st, 7));
            if (task->end == EDC_END_NONE)
            {
                if (task->life == EDC_LIFE_INTERRUPTED)
                    task->end = EDC_END_INTERRUPTED;
                else if (task->life == EDC_LIFE_CANCELLED)
                    task->end = EDC_END_STOPPED;
                else if (task->life == EDC_LIFE_ERROR)
                    task->end = EDC_END_FAILED;
            }
            if (task->end == EDC_END_BLOCKED)
            {
                const char *by = col_str(st, 8);
                if (by && *by)
                    task->blocked_by = strdup(by);
                task->blocked_end = edc_task_classify_end(col_str(st, 9));
            }
            task->fail = edc_task_classify_fail(col_str(st, 10));

            task->is_group_leader = sqlite3_column_int(st, 11) != 0;
            task->is_background = sqlite3_column_int(st, 12) != 0;
            task->tool_only = sqlite3_column_int(st, 13) != 0;
            task->has_deferred_vars = sqlite3_column_int(st, 14) != 0;
            task->memo_kind = edc_task_classify_memo_kind(col_str(st, 15));
            task->fold_cut_task_id = col_i64(st, 18);
            task->fold_cut_turn = (uint32_t)col_i64(st, 19);
            task->fold_cut_prefix = (uint32_t)col_i64(st, 20);
            task->memo_cut_task_id = col_i64(st, 21);
            task->memo_cut_turn = (uint32_t)col_i64(st, 22);
            task->memo_cut_prefix = (uint32_t)col_i64(st, 23);
            task->fork_point_completion_seq = (uint32_t)col_i64(st, 25);
            {
                const char *output = col_str(st, 26);
                if (output)
                    task->output = sdsnew(output);
            }
            task->completion_seq = (uint32_t)col_i64(st, 27);
            task->tokens_prompt = (int64_t)col_i64(st, 28);
            task->tokens_completion = (int64_t)col_i64(st, 29);
            task->tokens_total = (int64_t)col_i64(st, 30);
            task->tokens_cached = (int64_t)col_i64(st, 31);
            task->last_total_tokens = (int64_t)col_i64(st, 32);
            task->last_fold_epoch = (uint32_t)col_i64(st, 33);
            task->last_hist_len = (int)col_i64(st, 34);
            task->last_turn_len = (int)col_i64(st, 35);
            task->memo_seen_seq = (uint64_t)col_i64(st, 36);
            /* v8: the catalogue flag is its own column now, not `memo_seen_seq == 0`. */
            task->memo_baseline_done = sqlite3_column_int(st, 37) != 0;
            task->last_prefix_tokens = (uint64_t)col_i64(st, 38);
            {
                const char *rout = col_str(st, 39);
                if (rout)
                    task->redirect_stdout = sdsnew(rout);
                const char *rerr = col_str(st, 40);
                if (rerr)
                    task->redirect_stderr = sdsnew(rerr);
            }
            task->redirect_append = sqlite3_column_int(st, 41) != 0;
            task->redirect_stderr_append = sqlite3_column_int(st, 42) != 0;
            {
                const char *todos = col_str(st, 43);
                if (todos)
                {
                    cJSON *parsed = cJSON_Parse(todos);
                    if (parsed)
                        task->todos = parsed;
                }
            }
            task->todo_sync_gap = (int)col_i64(st, 44);

            load_task_put(task_map, task_id, task);
            if (task_count == task_cap)
            {
                size_t ncap = task_cap ? task_cap * 2 : 16;
                struct edc_task **grown = realloc(tasks, ncap * sizeof(*grown));
                if (!grown)
                {
                    edc_task_free(task);
                    continue;
                }
                tasks = grown;
                task_cap = ncap;
            }
            tasks[task_count++] = task;
        }
        sqlite3_finalize(st);
    }

    /* ── 3. fork chains ─────────────────────────────────────────────────
     * A parent pointer, so it only needs the tasks to exist. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "SELECT task_id,forked_from_task_id FROM tasks "
                               "WHERE session_id=? AND forked_from_task_id IS NOT NULL",
                               -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                struct edc_task *task = load_task_get(task_map, col_i64(st, 0));
                struct edc_task *parent = load_task_get(task_map, col_i64(st, 1));
                if (task && parent)
                    task->forked_from = parent;
            }
        }
        sqlite3_finalize(st);
    }

    /* ── 4. groups ──────────────────────────────────────────────────────
     *
     * The leader IS the task whose id equals group_id. An ORPHANED group has no such task
     * (its ex-leader belongs to no group any more, so it was not written), and then the
     * first member stands in -- but the group keeps the id from its own row. Deriving the id
     * from whichever task we fell back to would silently rename the group, and every message
     * row keyed on the old id would point at a group that no longer answers to it. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, "SELECT group_id,is_orphaned,kind,memory,fold_epoch,"
                                     "memory_created_at,memory_updated_at,memory_covers_to "
                                     "FROM groups WHERE session_id=? ORDER BY ord",
                               -1, &st, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            const int64_t group_id = col_i64(st, 0);
            const bool is_orph = sqlite3_column_int(st, 1) != 0;

            struct edc_task *leader = load_task_get(task_map, group_id);
            if (!leader && is_orph)
            {
                /* The first member, in the stored order. */
                sqlite3_stmt *m = NULL;
                if (sqlite3_prepare_v2(g_db,
                                       "SELECT task_id FROM tasks WHERE session_id=? AND group_id=? "
                                       "ORDER BY ord LIMIT 1", -1, &m, NULL) == SQLITE_OK)
                {
                    sqlite3_bind_int64(m, 1, (sqlite3_int64)session_id);
                    sqlite3_bind_int64(m, 2, (sqlite3_int64)group_id);
                    if (sqlite3_step(m) == SQLITE_ROW)
                        leader = load_task_get(task_map, col_i64(m, 0));
                }
                sqlite3_finalize(m);
            }
            if (!leader)
            {
                log_warn("load: group %" PRId64 " has no member to lead it -- skipped",
                         (int64_t)group_id);
                continue;
            }

            /* One task, one group: detach it from any other before attaching. */
            load_detach_from_groups(session, leader, leader->task_id, NULL);

            struct edc_task_group *group = edc_task_group_new(leader);
            if (!group)
                continue;

            if (group_id > 0 && group->group_id != group_id)
            {
                group->group_id = group_id;
                struct edc_task *m;
                LL_FOREACH2(group->members, m, group_next)
                    m->group_id = group_id;
            }
            if (is_orph)
                group->is_orphaned = true;
            group->kind = edc_task_group_classify_kind(col_str(st, 2));
            {
                const char *mem = col_str(st, 3);
                if (mem)
                    group->memory = sdsnew(mem);
            }
            group->fold_epoch = (uint32_t)col_i64(st, 4);
            group->memory_created_at = (uint64_t)col_i64(st, 5);
            group->memory_updated_at = (uint64_t)col_i64(st, 6);
            group->memory_covers_to = (uint64_t)col_i64(st, 7);

            /* members, in the stored order; the leader is already on the list */
            {
                sqlite3_stmt *m = NULL;
                if (sqlite3_prepare_v2(g_db, "SELECT task_id FROM tasks WHERE session_id=? AND "
                                             "group_id=? ORDER BY ord", -1, &m, NULL) == SQLITE_OK)
                {
                    sqlite3_bind_int64(m, 1, (sqlite3_int64)session_id);
                    sqlite3_bind_int64(m, 2, (sqlite3_int64)group_id);
                    while (sqlite3_step(m) == SQLITE_ROW)
                    {
                        const int64_t mid = col_i64(m, 0);
                        if (mid <= 0 || mid == leader->task_id)
                            continue;
                        struct edc_task *mt = load_task_get(task_map, mid);
                        if (!mt)
                            continue;
                        load_detach_from_groups(session, mt, mid, group);
                        edc_task_group_add_task(group, mt);
                    }
                }
                sqlite3_finalize(m);
            }

            /* The tracked-file lists, in the order they were written. Restored through
             * edc_fold_files_add_*(), not by filling the arrays here: the module owns what
             * "read" and "modified" mean (a path edited after being read belongs ONLY in
             * modified), and a second copy of that rule is how the persistence layer got the
             * shape wrong in the first place. */
            {
                sqlite3_stmt *f = NULL;
                if (sqlite3_prepare_v2(g_db,
                                       "SELECT kind,path FROM group_fold_files "
                                       "WHERE session_id=? AND group_id=? ORDER BY kind,ord",
                                       -1, &f, NULL) == SQLITE_OK)
                {
                    sqlite3_bind_int64(f, 1, (sqlite3_int64)session_id);
                    sqlite3_bind_int64(f, 2, (sqlite3_int64)group_id);
                    while (sqlite3_step(f) == SQLITE_ROW)
                    {
                        const char *kind = col_str(f, 0);
                        const char *path = col_str(f, 1);
                        if (!kind || !path || !*path)
                            continue;
                        if (!group->fold_files)
                            group->fold_files = edc_fold_files_new();
                        if (strcmp(kind, "modified") == 0)
                            edc_fold_files_add_modified(group->fold_files, path);
                        else if (strcmp(kind, "read") == 0)
                            edc_fold_files_add_read(group->fold_files, path);
                    }
                }
                sqlite3_finalize(f);
            }

            edc_session_add_group(session, group);
            load_group_put(group_map, group_id, group);
        }
        sqlite3_finalize(st);
    }

    /* ── 5. a fold task's sink group is a pointer; the row carries its id ──── */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "SELECT task_id,sink_group_id FROM tasks "
                               "WHERE session_id=? AND sink_group_id IS NOT NULL", -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                struct edc_task *task = load_task_get(task_map, col_i64(st, 0));
                if (task)
                    task->sink_group = load_group_get(group_map, col_i64(st, 1));
            }
        }
        sqlite3_finalize(st);
    }

    /* ── 6. dependency edges ────────────────────────────────────────────
     *
     * A source in another session is not in our map. It is recorded as a PENDING edge for
     * the app-level reconcile instead of being dropped: without it the target's deps_total
     * would omit the dependency and it could be resumed before the source completes. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "SELECT target_task_id,source_task_id,type,completed,cancelled,injected "
                               "FROM task_deps WHERE target_task_id IN "
                               "(SELECT task_id FROM tasks WHERE session_id=?) ORDER BY target_task_id,ord",
                               -1, &st, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            const int64_t tid = col_i64(st, 0);
            const int64_t src_id = col_i64(st, 1);
            const edc_task_dep_type dt = edc_task_classify_dep_type(col_str(st, 2));
            const bool completed = sqlite3_column_int(st, 3) != 0;
            const bool cancelled = sqlite3_column_int(st, 4) != 0;
            const bool injected = sqlite3_column_int(st, 5) != 0;

            struct edc_task *target = load_task_get(task_map, tid);
            if (!target)
                continue;
            struct edc_task *source = load_task_get(task_map, src_id);
            if (!source)
            {
                if (sched)
                    edc_scheduler_add_pending_dep(sched, tid, src_id, dt, completed, cancelled, injected);
                continue;
            }

            edc_task_add_dependency(target, source, dt);
            if (completed)
                edc_task_on_dep_completed(target, source);
            if (cancelled)
                edc_task_on_dep_cancelled(target, source);
            if (injected)
            {
                struct edc_task_dep *dep;
                LL_FOREACH(target->deps, dep)
                {
                    if (dep->source == source)
                    {
                        dep->injected = true;
                        break;
                    }
                }
            }
        }
        sqlite3_finalize(st);
    }

    load_reconcile_deps(tasks, task_count);

    /* ── 7. the resume ledger ───────────────────────────────────────────── */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "SELECT task_id,call_id,name,args,status FROM task_tool_calls "
                               "WHERE task_id IN (SELECT task_id FROM tasks WHERE session_id=?) "
                               "ORDER BY task_id,ord", -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                struct edc_task *task = load_task_get(task_map, col_i64(st, 0));
                if (!task)
                    continue;
                const char *args = col_str(st, 3);
                cJSON *parsed = args ? cJSON_Parse(args) : NULL;
                struct edc_tool_call_record *rec =
                    edc_task_tool_call_add(task, col_str(st, 1), col_str(st, 2), parsed);
                if (rec)
                {
                    const char *status = col_str(st, 4);
                    rec->status = (status && strcmp(status, "executing") == 0) ? TOOL_CALL_EXECUTING
                                                                              : TOOL_CALL_PENDING;
                }
                else if (parsed)
                {
                    cJSON_Delete(parsed);
                }
            }
        }
        sqlite3_finalize(st);
    }

    /* ── 8. turns ───────────────────────────────────────────────────────
     * The ordinal IS the storage key the message rows were written under, so it is taken
     * from the row rather than re-derived. is_active marks the in-flight turn, which hangs
     * off the task as active_turn instead of the queue. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "SELECT t.task_id,t.turn_ord,t.folded_prefix,t.memo_prefix,t.is_active,"
                               "t.msg_count FROM turns t JOIN tasks k ON k.task_id=t.task_id "
                               "WHERE k.session_id=? ORDER BY t.task_id,t.turn_ord", -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                struct edc_task *task = load_task_get(task_map, col_i64(st, 0));
                if (!task)
                    continue;
                const int ord = (int)col_i64(st, 1);
                struct edc_turn *turn = edc_turn_new_for(session_id, task->group_id, task->task_id, ord);
                if (!turn)
                    continue;
                turn->folded_prefix = (int)col_i64(st, 2);
                turn->memo_prefix = (int)col_i64(st, 3);
                turn->msg_count = (int)col_i64(st, 5);

                if (sqlite3_column_int(st, 4) != 0)
                    task->active_turn = turn;
                else
                    edc_task_add_turn(task, turn);
            }
        }
        sqlite3_finalize(st);
    }

    /* ── 9. the session's pointers ──────────────────────────────────────── */
    session->active_task = load_task_get(task_map, active_task_id);
    /* The foreground group is a session attribute; the scheduler keeps no copy, so there is
     * no "current view" to restore here. */
    session->foreground_group = load_group_get(group_map, foreground_group_id);

    /* ── 10. the derived view, built the same way every other path builds it ── */
    {
        struct edc_task_group *g;
        DL_FOREACH(session->groups, g)
            edc_task_group_rebuild_history(g);
    }

    /* ── 11. session memory rows ────────────────────────────────────────── */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, "SELECT seq,ts,entry_id,kind,title,summary,changed FROM memo_events "
                                     "WHERE session_id=? ORDER BY seq", -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            while (sqlite3_step(st) == SQLITE_ROW)
                edc_session_memory_restore_event(session->memo, (uint64_t)col_i64(st, 0),
                                                 (uint64_t)col_i64(st, 1), col_str(st, 2), col_str(st, 3),
                                                 col_str(st, 4), col_str(st, 5),
                                                 (uint32_t)sqlite3_column_int64(st, 6));
        }
        sqlite3_finalize(st);
    }
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db, "SELECT entry_id,title,summary,detail,sources,proposed_by,"
                                     "created_seq,updated_seq,created_at,updated_at,superseded "
                                     "FROM memo_entries WHERE session_id=? ORDER BY entry_id",
                               -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            while (sqlite3_step(st) == SQLITE_ROW)
                edc_session_memory_restore_entry(session->memo, col_str(st, 0), col_str(st, 1),
                                                 col_str(st, 2), col_str(st, 3), col_str(st, 4),
                                                 col_str(st, 5), (uint64_t)col_i64(st, 6),
                                                 (uint64_t)col_i64(st, 7), (uint64_t)col_i64(st, 8),
                                                 (uint64_t)col_i64(st, 9), sqlite3_column_int(st, 10) != 0);
        }
        sqlite3_finalize(st);
    }

    ok = true;

done:
    load_task_map_free(task_map);
    load_group_map_free(group_map);
    free(tasks);
    if (!ok)
        log_error("state_store: load failed for %s", edc_id_str(session_id));
    return ok;
}

bool edc_state_store_insert_task_new(int64_t session_id, int64_t group_id, int64_t *out_task_id)
{
    if (out_task_id)
        *out_task_id = 0;
    if (!g_db || session_id <= 0 || !out_task_id)
        return false;

    if (!exec_sql(g_db, "BEGIN IMMEDIATE", "new task BEGIN"))
        return false;

    int64_t id = 0;
    {
        /* The session row must exist first: tasks.session_id references it, and
         * foreign_keys is ON. INSERT OR IGNORE because a live session is usually already
         * there (the flush creates it); this only covers the very first task of a workspace
         * or a task created before its session was ever persisted. */
        sqlite3_stmt *st = NULL;
        bool ok = false;
        if (sqlite3_prepare_v2(g_db,
                               "INSERT OR IGNORE INTO sessions(session_id,name,updated_at) "
                               "VALUES(?,?,?)",
                               -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            /* name: a placeholder here; the real name arrives with the metadata flush.
             * An id renders one way (edc_id_str), which is the same fallback
             * edc_session_new_with_workdir uses for an unnamed session. */
            bind_str(st, 2, edc_id_str(session_id));
            sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
        if (!ok)
        {
            exec_sql(g_db, "ROLLBACK", "new task rollback");
            return false;
        }
    }

    {
        sqlite3_stmt *st = NULL;
        bool ok = false;
        /* group_id starts at 0: a task that leads its own group cannot know it before the
         * row exists. The second statement closes that window inside the same transaction. */
        if (sqlite3_prepare_v2(g_db,
                               "INSERT INTO tasks(session_id,group_id,updated_at) VALUES(?,0,?)",
                               -1, &st, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
        if (!ok)
        {
            exec_sql(g_db, "ROLLBACK", "new task rollback");
            return false;
        }
        id = (int64_t)sqlite3_last_insert_rowid(g_db);
    }

    {
        const int64_t want_group = (group_id > 0) ? group_id : id;
        sqlite3_stmt *st = NULL;
        bool ok = false;
        if (sqlite3_prepare_v2(g_db, "UPDATE tasks SET group_id=? WHERE task_id=?", -1, &st, NULL) ==
            SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)want_group);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
        if (!ok)
        {
            exec_sql(g_db, "ROLLBACK", "new task rollback");
            return false;
        }
    }

    if (!exec_sql(g_db, "COMMIT", "new task COMMIT"))
    {
        exec_sql(g_db, "ROLLBACK", "new task rollback");
        return false;
    }
    *out_task_id = id;
    return true;
}

bool edc_state_store_has_session(int64_t session_id)
{
    if (!g_db || session_id <= 0)
        return false;
    return session_exists(session_id);
}

/* ── The session list, and which one is active ─────────────────────────────── */

void edc_session_rows_free(struct edc_session_row *rows, int n)
{
    if (!rows)
        return;
    for (int i = 0; i < n; i++)
    {
        free(rows[i].name);
    }
    free(rows);
}

bool edc_state_store_list_sessions(struct edc_session_row **out, int *n)
{
    if (out)
        *out = NULL;
    if (n)
        *n = 0;
    if (!g_db)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "SELECT session_id, name, coalesce(created_at,0), "
                           "coalesce(last_active,0), is_active "
                           "FROM sessions ORDER BY created_at, session_id",
                           -1, &st, NULL) != SQLITE_OK)
        return false;

    struct edc_session_row *rows = NULL;
    int cnt = 0, cap = 0;
    bool ok = true;
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        if (cnt == cap)
        {
            const int ncap = cap ? cap * 2 : 8;
            struct edc_session_row *grown = realloc(rows, (size_t)ncap * sizeof(*grown));
            if (!grown)
            {
                ok = false;
                break;
            }
            rows = grown;
            cap = ncap;
        }
        struct edc_session_row *r = &rows[cnt++];
        const char *id = col_str(st, 0);
        const char *name = col_str(st, 1);
        r->session_id = id ? (int64_t)strtoll(id, NULL, 10) : 0;
        r->name = strdup(name ? name : "");
        r->created_at = (uint64_t)sqlite3_column_int64(st, 2);
        r->last_active = (uint64_t)sqlite3_column_int64(st, 3);
        r->is_active = sqlite3_column_int(st, 4) != 0;
    }
    sqlite3_finalize(st);

    if (!ok)
    {
        edc_session_rows_free(rows, cnt);
        return false;
    }
    if (out)
        *out = rows;
    else
        edc_session_rows_free(rows, cnt);
    if (n)
        *n = cnt;
    return true;
}

bool edc_state_store_set_active_session(int64_t session_id)
{
    if (!g_db || session_id <= 0)
        return false;

    /* Clear-then-set in one transaction: the partial unique index rejects the moment two
     * rows carry is_active=1, so the two statements cannot be allowed to straddle a commit
     * that another reader could see. */
    if (!exec_sql(g_db, "BEGIN IMMEDIATE", "set_active BEGIN"))
        return false;
    if (!exec_sql(g_db, "UPDATE sessions SET is_active=0 WHERE is_active<>0", "set_active clear"))
    {
        exec_sql(g_db, "ROLLBACK", "set_active rollback");
        return false;
    }
    {
        sqlite3_stmt *st = NULL;
        bool ok = false;
        if (sqlite3_prepare_v2(g_db, "UPDATE sessions SET is_active=1 WHERE session_id=?", -1, &st, NULL) ==
            SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(g_db) > 0;
            sqlite3_finalize(st);
        }
        if (!ok)
        {
            exec_sql(g_db, "ROLLBACK", "set_active rollback");
            return false;
        }
    }
    if (!exec_sql(g_db, "COMMIT", "set_active COMMIT"))
    {
        exec_sql(g_db, "ROLLBACK", "set_active rollback");
        return false;
    }
    return true;
}

bool edc_state_store_insert_session_new(const char *name, int64_t *out_session_id)
{
    if (out_session_id)
        *out_session_id = 0;
    if (!g_db || !out_session_id)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "INSERT INTO sessions(name,created_at,last_active,updated_at) "
                           "VALUES(?,?,?,?)",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    const sqlite3_int64 now = (sqlite3_int64)time(NULL);
    bind_str(st, 1, (name && *name) ? name : "");
    sqlite3_bind_int64(st, 2, now);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, now);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        return false;
    *out_session_id = (int64_t)sqlite3_last_insert_rowid(g_db);
    return true;
}

bool edc_state_store_ensure_session(int64_t session_id, const char *name, uint64_t created_at)
{
    if (!g_db || session_id <= 0)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db,
                           "INSERT OR IGNORE INTO sessions(session_id,name,created_at,last_active,"
                           "updated_at) VALUES(?,?,?,?,?)",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    {
        /* A session with no name shows its id, which is a number now. */
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "%" PRId64, session_id);
        bind_str(st, 2, (name && *name) ? name : idbuf);
    }
    sqlite3_bind_int64(st, 3, (sqlite3_int64)created_at);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)created_at);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)time(NULL));
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool edc_state_store_delete_session(int64_t session_id)
{
    if (!g_db || session_id <= 0)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM sessions WHERE session_id=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* ── message JSON <-> columns ────────────────────────────────────────────────
 *
 * A message in the C world is ONE cJSON object with everything in it: wire fields plus the
 * metadata the store keeps separately (id, ts, render, mid, payload, synthetic). In the
 * database those metadata fields are columns and `body` holds the rest, so every write
 * splits the object and every read merges it back.
 *
 * The split is defined by a NAME LIST, not by a wire allowlist: anything the store does not
 * know stays in `body` and therefore survives a round trip untouched. That is what lets a
 * provider field we have never heard of (reasoning_content today, something else tomorrow)
 * ride along without a schema change.
 *
 * `role` is on the list even though it IS a wire field: it also has a column (queries filter
 * on it), and a value stored in two places is a value that will eventually disagree. The
 * column wins on read, so keeping the copy inside `body` would buy nothing but a chance for
 * the two to drift -- exactly the mistake v3 exists to undo.
 */
static const char *const kStoreOnlyMsgKeys[] = {"id",     "ts",    "render", "mid",
                                                "payload", "synthetic", "role", NULL};

static bool msg_key_is_store_only(const char *k)
{
    if (!k)
        return false;
    for (int i = 0; kStoreOnlyMsgKeys[i]; i++)
        if (strcmp(k, kStoreOnlyMsgKeys[i]) == 0)
            return true;
    return false;
}

/* Serialize the message WITHOUT its store-only keys. Caller frees with cJSON_free. */
static char *msg_body_text(const cJSON *msg)
{
    cJSON *copy = cJSON_Duplicate((cJSON *)msg, 1);
    if (!copy)
        return NULL;
    /* Collect first, delete after: mutating the list while iterating it skips children. */
    sds drop = sdsempty();
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, copy)
    {
        if (msg_key_is_store_only(child->string))
            drop = sdscatprintf(drop, "%s\n", child->string);
    }
    int n = 0;
    char **names = sdssplitlen(drop, (ssize_t)sdslen(drop), "\n", 1, &n);
    for (int i = 0; i < n; i++)
        if (names[i] && *names[i])
            cJSON_DeleteItemFromObject(copy, names[i]);
    sdsfreesplitres(names, n);
    sdsfree(drop);

    char *txt = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    return txt;
}

/* The payload map as text (NULL when absent). */
static char *msg_payload_text(const cJSON *msg)
{
    cJSON *p = cJSON_GetObjectItem((cJSON *)msg, "payload");
    if (!p)
        return NULL;
    return cJSON_PrintUnformatted(p);
}

/* Rebuild the full message object from a row: body first, then the metadata columns, which
 * take precedence (they are the ones the runtime reads back). */
static cJSON *msg_from_columns(const char *body, const char *id, const char *role, int64_t ts,
                               const char *render, int64_t synthetic, const char *payload,
                               const char *mid){
    cJSON *msg = body ? cJSON_Parse(body) : NULL;
    if (!msg || !cJSON_IsObject(msg))
    {
        if (msg)
            cJSON_Delete(msg);
        msg = cJSON_CreateObject();
    }

    if (id && *id)
    {
        cJSON_DeleteItemFromObject(msg, "id");
        cJSON_AddStringToObject(msg, "id", id);
    }
    if (role && *role)
    {
        cJSON_DeleteItemFromObject(msg, "role");
        cJSON_AddStringToObject(msg, "role", role);
    }
    if (ts > 0)
    {
        cJSON_DeleteItemFromObject(msg, "ts");
        sds disp = ts_to_display(ts);
        cJSON_AddStringToObject(msg, "ts", disp ? disp : "");
        sdsfree(disp);
    }
    if (render && *render)
    {
        cJSON_DeleteItemFromObject(msg, "render");
        cJSON_AddStringToObject(msg, "render", render);
    }
    if (synthetic)
    {
        cJSON_DeleteItemFromObject(msg, "synthetic");
        cJSON_AddTrueToObject(msg, "synthetic");
    }
    if (mid && *mid)
    {
        cJSON_DeleteItemFromObject(msg, "mid");
        cJSON_AddStringToObject(msg, "mid", mid);
    }
    if (payload && *payload)
    {
        cJSON *p = cJSON_Parse(payload);
        if (p)
        {
            cJSON_DeleteItemFromObject(msg, "payload");
            cJSON_AddItemToObject(msg, "payload", p);
        }
    }
    return msg;
}

/* Build one message from the row the statement is positioned on (column order: see
 * MSG_SELECT_COLS). */
static cJSON *msg_from_row(sqlite3_stmt *st, int base)
{
    return msg_from_columns(col_str(st, base + 0), col_str(st, base + 1), col_str(st, base + 2),
                            sqlite3_column_int64(st, base + 3), col_str(st, base + 4),
                            sqlite3_column_int64(st, base + 5), col_str(st, base + 6),
                            col_str(st, base + 7));
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Message rows: the runtime read/write path
 *
 *  These are deliberately NOT built on edc_state_serialize(): that function exists to
 *  produce a whole-session snapshot, and using it here is exactly what this stage
 *  removes. A message is a row; appending one is one INSERT.
 * ══════════════════════════════════════════════════════════════════════════ */

bool edc_state_ensure_turn(int64_t session_id, int64_t group_id, int64_t task_id,
                           int turn_ord)
{
    if (!g_db || task_id <= 0)
        return false;
    /* A turn built without a session (tests, and any caller that only carries a task id)
     * still needs an ancestor row for the FK chain. One shared placeholder keeps that
     * possible without pretending it is a real session; production always passes one. */
    if (session_id <= 0)
        session_id = EDC_SESSION_ID_PLACEHOLDER;

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
        switch (i)
        {
        case 0: /* sessions(session_id) */
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            break;
        case 1: /* groups(session_id, group_id) */
            sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)group_id);
            break;
        case 2: /* tasks(task_id, session_id, group_id) */
            sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)session_id);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)group_id);
            break;
        default: /* turns(task_id, turn_ord) */
            sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
            sqlite3_bind_int(st, 2, turn_ord);
            break;
        }

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
static void turn_recount(int64_t task_id, int turn_ord)
{
    if (!g_db || task_id <= 0)
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
    sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
    sqlite3_bind_int(st, 2, turn_ord);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

bool edc_msg_append(int64_t task_id, int turn_ord, int msg_ord, const struct cJSON *msg,
                    int64_t tokens)
{
    if (!g_db || task_id <= 0 || !msg || msg_ord < 0)
        return false;

    /* body = the message minus the metadata that lives in columns (see kStoreOnlyMsgKeys).
     * Splitting here is what keeps the two representations from drifting: a read merges the
     * columns back, so there is exactly one place a value is stored. */
    char *body = msg_body_text(msg);
    if (!body)
        return false;
    char *payload = msg_payload_text(msg);

    const cJSON *role = cJSON_GetObjectItem((cJSON *)msg, "role");
    const cJSON *mid = cJSON_GetObjectItem((cJSON *)msg, "id");
    const cJSON *ts = cJSON_GetObjectItem((cJSON *)msg, "ts");
    const cJSON *render = cJSON_GetObjectItem((cJSON *)msg, "render");
    const cJSON *synthetic = cJSON_GetObjectItem((cJSON *)msg, "synthetic");
    const cJSON *midf = cJSON_GetObjectItem((cJSON *)msg, "mid");

    sqlite3_stmt *st = NULL;
    /* OR REPLACE on the natural key: re-appending the same position (a replayed event, a
     * retry) overwrites rather than duplicating. The row's `id` primary key is rewritten
     * too, so a changed message cannot leave two identities for one position.
     *
     * session_id / group_id come from the task row (edc_state_ensure_turn made sure it is
     * there), so this function needs only the task key. */
    const char *sql =
        "INSERT OR REPLACE INTO messages(id,session_id,group_id,task_id,turn_ord,msg_ord,"
        "role,mid,ts,render,synthetic,payload,tokens,body) "
        "VALUES(?,"
        " (SELECT session_id FROM tasks WHERE task_id=?2),"
        " (SELECT group_id   FROM tasks WHERE task_id=?2),"
        " ?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)";
    bool ok = false;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK)
    {
        const char *id = cJSON_GetStringValue((cJSON *)mid);
        sds fallback = NULL;
        if (!id || !*id)
        {
            /* No identity on the message: a deterministic positional one, so a re-flush
             * cannot insert a second copy of the same message. */
            fallback = sdscatprintf(sdsempty(), "%s#%d.%d", edc_id_str(task_id), turn_ord, msg_ord);
            id = fallback;
        }
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)task_id);
        sqlite3_bind_int(st, 3, turn_ord);
        sqlite3_bind_int(st, 4, msg_ord);
        bind_str(st, 5, cJSON_GetStringValue((cJSON *)role));
        bind_str(st, 6, cJSON_GetStringValue((cJSON *)midf));
        sqlite3_bind_int64(st, 7, ts_from_display(cJSON_GetStringValue((cJSON *)ts)));
        bind_str(st, 8, cJSON_GetStringValue((cJSON *)render));
        sqlite3_bind_int(st, 9, (synthetic && cJSON_IsTrue((cJSON *)synthetic)) ? 1 : 0);
        bind_str(st, 10, payload);
        sqlite3_bind_int64(st, 11, (sqlite3_int64)tokens);
        sqlite3_bind_text(st, 12, body, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (fallback)
            sdsfree(fallback);
    }
    cJSON_free(body);
    if (payload)
        cJSON_free(payload);
    if (ok)
        turn_recount(task_id, turn_ord);
    return ok;
}

/* ── Message rows end ─────────────────────────────────────────────────────── */

struct cJSON *edc_msg_get(int64_t task_id, int turn_ord, int msg_ord)
{
    if (!g_db || task_id <= 0 || msg_ord < 0)
        return NULL;

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT " MSG_SELECT_COLS
             " FROM messages WHERE task_id=? AND turn_ord=? AND msg_ord=?");
    sqlite3_stmt *st = NULL;
    struct cJSON *out = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
        sqlite3_bind_int(st, 2, turn_ord);
        sqlite3_bind_int(st, 3, msg_ord);
        if (sqlite3_step(st) == SQLITE_ROW)
            out = msg_from_row(st, 0);
        sqlite3_finalize(st);
    }
    return out;
}

int64_t edc_msg_turn_tokens_from(int64_t task_id, int turn_ord, int from)
{
    if (!g_db || task_id <= 0)
        return -1;

    sqlite3_stmt *st = NULL;
    int64_t total = 0;
    if (sqlite3_prepare_v2(g_db,
                           "SELECT coalesce(sum(tokens),0) FROM messages "
                           "WHERE task_id=? AND turn_ord=? AND msg_ord>=?",
                           -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
        sqlite3_bind_int(st, 2, turn_ord);
        sqlite3_bind_int(st, 3, from > 0 ? from : 0);
        if (sqlite3_step(st) == SQLITE_ROW)
            total = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return total;
}

static bool msg_run_delete(const char *sql, int64_t task_id, int turn_ord, int arg3)
{
    if (!g_db || task_id <= 0)
        return false;
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
        sqlite3_bind_int(st, 2, turn_ord);
        if (arg3 >= 0)
            sqlite3_bind_int(st, 3, arg3);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    return ok;
}

bool edc_msg_turn_truncate(int64_t task_id, int turn_ord, int new_len)
{
    const bool ok = msg_run_delete("DELETE FROM messages WHERE task_id=? AND turn_ord=? AND msg_ord>=?",
                                   task_id, turn_ord, new_len < 0 ? 0 : new_len);
    if (ok)
        turn_recount(task_id, turn_ord);
    return ok;
}

bool edc_msg_turn_move(int64_t old_task, int old_ord, int64_t new_session,
                       int64_t new_group, int64_t new_task, int new_ord)
{
    if (!g_db || old_task <= 0 || new_task <= 0)
        return false;
    if (old_task == new_task && old_ord == new_ord)
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
        sqlite3_bind_int64(st, 1, (sqlite3_int64)new_task);
        sqlite3_bind_int(st, 2, new_ord);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)old_task);
        sqlite3_bind_int(st, 4, old_ord);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    /* Messages FIRST: while they still point at the old turn row, changing the turns key
     * would violate messages' foreign key.
     *
     * session_id / group_id are re-derived from the DESTINATION task in the same statement.
     * They are denormalized copies of that task's owner, so leaving them alone would make
     * the row claim the old group while sitting under the new one -- silently, and only
     * for the reads that filter on those columns. */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_db,
                               "UPDATE OR REPLACE messages SET task_id=?1, turn_ord=?2, "
                               "  session_id=(SELECT session_id FROM tasks WHERE task_id=?1), "
                               "  group_id=(SELECT group_id FROM tasks WHERE task_id=?1) "
                               "WHERE task_id=?3 AND turn_ord=?4",
                               -1, &st, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)new_task);
        sqlite3_bind_int(st, 2, new_ord);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)old_task);
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
        sqlite3_bind_int64(st, 1, (sqlite3_int64)old_task);
        sqlite3_bind_int(st, 2, old_ord);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    turn_recount(new_task, new_ord);
    return true;
}

bool edc_msg_span_load(int64_t task_id, int turn_ord, int from, int to, struct edc_msg_span *out)
{
    if (!out)
        return false;
    out->items = NULL;
    out->n = 0;

    if (!g_db || task_id <= 0)
        return false;
    if (from < 0)
        from = 0;

    char sql[512];
    snprintf(sql, sizeof(sql),
             (to < 0) ? "SELECT msg_ord," MSG_SELECT_COLS
                        " FROM messages WHERE task_id=? AND turn_ord=? AND msg_ord>=? ORDER BY msg_ord"
                      : "SELECT msg_ord," MSG_SELECT_COLS
                        " FROM messages WHERE task_id=? AND turn_ord=? "
                        "AND msg_ord>=? AND msg_ord<? ORDER BY msg_ord");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
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
        const char *body = col_str(st, 1);
        out->items[want - 1] = body ? msg_from_row(st, 1) : NULL;
        if (want > n)
            n = want;
    }
    sqlite3_finalize(st);
    out->n = n;
    return true;
}

/* ── Addressing messages by id ─────────────────────────────────────────────── */

bool edc_msg_ids_for_turn(int64_t task_id, int turn_ord, int from, int to,
                          struct edc_msg_ids *out)
{
    if (!out)
        return false;
    out->ids = NULL;
    out->n = 0;
    if (!g_db || task_id <= 0)
        return false;
    if (from < 0)
        from = 0;

    char sql[256];
    snprintf(sql, sizeof(sql),
             (to < 0) ? "SELECT id FROM messages WHERE task_id=? AND turn_ord=? AND msg_ord>=? "
                        "ORDER BY msg_ord"
                      : "SELECT id FROM messages WHERE task_id=? AND turn_ord=? "
                        "AND msg_ord>=? AND msg_ord<? ORDER BY msg_ord");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
    sqlite3_bind_int(st, 2, turn_ord);
    sqlite3_bind_int(st, 3, from);
    if (to >= 0)
        sqlite3_bind_int(st, 4, to);

    int cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const char *id = col_str(st, 0);
        if (!id || !*id)
            continue;
        if (out->n == cap)
        {
            const int ncap = cap ? cap * 2 : 16;
            char **grown = realloc(out->ids, (size_t)ncap * sizeof(*grown));
            if (!grown)
                break;
            out->ids = grown;
            cap = ncap;
        }
        out->ids[out->n++] = strdup(id);
    }
    sqlite3_finalize(st);
    return true;
}

void edc_msg_ids_free(struct edc_msg_ids *s)
{
    if (!s || !s->ids)
        return;
    for (int i = 0; i < s->n; i++)
        free(s->ids[i]);
    free(s->ids);
    s->ids = NULL;
    s->n = 0;
}

struct cJSON *edc_msg_get_by_id(const char *id)
{
    if (!g_db || !id || !*id)
        return NULL;

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT " MSG_SELECT_COLS " FROM messages WHERE id=?");
    sqlite3_stmt *st = NULL;
    struct cJSON *out = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            out = msg_from_row(st, 0);
        sqlite3_finalize(st);
    }
    return out;
}

/* Stage the ids in a temp table so SQLite can do the ordering. Caller holds no lock: this
 * runs on the scheduler thread like every other store call. */
static bool msg_ids_stage(const char *const *ids, int n)
{
    if (!exec_sql(g_db,
                  "CREATE TEMP TABLE IF NOT EXISTS want_msg(pos INTEGER PRIMARY KEY, msg_id TEXT);"
                  "DELETE FROM want_msg;",
                  "want_msg stage"))
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO want_msg(pos,msg_id) VALUES(?,?)", -1, &st, NULL) !=
        SQLITE_OK)
        return false;
    for (int i = 0; i < n; i++)
    {
        if (!ids[i] || !*ids[i])
            continue;
        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, i);
        sqlite3_bind_text(st, 2, ids[i], -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE)
        {
            sqlite3_finalize(st);
            return false;
        }
    }
    sqlite3_finalize(st);
    return true;
}

struct cJSON *edc_msg_load_by_ids(const char *const *ids, int n)
{
    if (!g_db || !ids || n <= 0)
        return NULL;
    if (!msg_ids_stage(ids, n))
        return NULL;

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT " MSG_SELECT_COLS
             " FROM want_msg w JOIN messages m ON m.id=w.msg_id ORDER BY w.pos");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        return NULL;

    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        cJSON *msg = msg_from_row(st, 0);
        if (msg)
            cJSON_AddItemToArray(arr, msg);
    }
    sqlite3_finalize(st);
    return arr;
}

bool edc_msg_role_by_id(const char *id, char *buf, size_t n)
{
    if (buf && n)
        buf[0] = '\0';
    if (!g_db || !id || !*id || !buf || n == 0)
        return false;

    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(g_db, "SELECT role FROM messages WHERE id=?", -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            const char *role = col_str(st, 0);
            snprintf(buf, n, "%s", role ? role : "");
            found = true;
        }
        sqlite3_finalize(st);
    }
    return found;
}

int64_t edc_msg_tokens_by_ids(const char *const *ids, int n)
{
    if (!g_db)
        return -1;
    if (!ids || n <= 0)
        return 0;
    if (!msg_ids_stage(ids, n))
        return -1;

    sqlite3_stmt *st = NULL;
    int64_t total = 0;
    if (sqlite3_prepare_v2(g_db,
                           "SELECT coalesce(sum(m.tokens),0) FROM want_msg w "
                           "JOIN messages m ON m.id=w.msg_id",
                           -1, &st, NULL) == SQLITE_OK)
    {
        if (sqlite3_step(st) == SQLITE_ROW)
            total = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return total;
}

bool edc_state_store_forget(int64_t session_id)
{
    if (!g_db || session_id <= 0)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM sessions WHERE session_id=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)session_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

char *edc_state_store_session_for_task(int64_t task_id)
{
    if (!g_db || task_id <= 0)
        return NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, "SELECT session_id FROM tasks WHERE task_id=?", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)task_id);
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
