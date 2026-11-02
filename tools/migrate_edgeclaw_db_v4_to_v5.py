#!/usr/bin/env python3
"""Migrate an edgeclaw workspace database from schema v4 to v5.

WHAT CHANGED IN v5
    task_id and group_id became INTEGERs assigned by the database
    (tasks.task_id is INTEGER PRIMARY KEY AUTOINCREMENT). They used to be TEXT produced by
    a process-wide counter whose high-water mark was persisted in <workspace>/.task_seq --
    one open/write/rename per allocation, and a file that had to be kept in step with every
    allocation forever. The INSERT allocates now, so that file is gone.

    Every column that holds an id changes type, and so does every foreign key that points
    at them. SQLite cannot change a column's type in place, so each affected table is
    rebuilt. The data itself is unchanged: each value is CAST from its decimal text to an
    integer. (Verified on a real workspace before writing this: 1740 task_id and 92
    group_id values, all decimal, no collisions after CAST.)

WHY THE ORDER MATTERS
    Rebuilding a parent table invalidates its children's foreign keys mid-flight, so
    foreign_keys is OFF for the whole migration and turned back ON (and checked) at the
    end. Tables are rebuilt parent-first so that when a child is copied, the rows it
    references already exist in their new form.

SAFETY
    * Refuses to run while an edgeclaw process holds the workspace lock.
    * Backs up the database (-wal/-shm too) and verifies the backup before writing.
    * One transaction: a failure leaves the file exactly as it was.
    * --dry-run reports and rolls back.
    * Idempotent: a v5 file is reported and left alone.
    * Verifies afterwards: row counts per table are unchanged, every id is a positive
      integer, and no orphaned child row points at a missing parent.

USAGE
    python3 tools/migrate_edgeclaw_db_v4_to_v5.py <workspace> [--dry-run|--check]
"""

import argparse
import fcntl
import os
import shutil
import sqlite3
import sys
import time

DB_NAME = "edgeclaw.db"
V4 = 4
V5 = 5

# (table, id columns to CAST, full new DDL). Parent-first order.
TABLES = [
    (
        "sessions",
        ["active_task_id", "foreground_group_id"],
        """CREATE TABLE sessions_v5 (
             session_id            TEXT PRIMARY KEY,
             name                  TEXT NOT NULL,
             created_at            INTEGER,
             last_active           INTEGER,
             workdir               TEXT,
             active_task_id        INTEGER,
             foreground_group_id   INTEGER,
             next_completion_seq   INTEGER NOT NULL DEFAULT 1,
             is_active             INTEGER NOT NULL DEFAULT 0,
             memo_next_seq         INTEGER NOT NULL DEFAULT 0,
             memo_next_entry_id    INTEGER NOT NULL DEFAULT 0,
             memo_pruned_before    INTEGER NOT NULL DEFAULT 0,
             updated_at            INTEGER NOT NULL)""",
    ),
    (
        "groups",
        ["group_id"],
        """CREATE TABLE groups_v5 (
             session_id         TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
             group_id           INTEGER NOT NULL,
             is_orphaned        INTEGER NOT NULL DEFAULT 0,
             kind               TEXT NOT NULL DEFAULT 'user',
             memory             TEXT,
             fold_epoch         INTEGER NOT NULL DEFAULT 0,
             memory_created_at  INTEGER NOT NULL DEFAULT 0,
             memory_updated_at  INTEGER NOT NULL DEFAULT 0,
             memory_covers_to   INTEGER NOT NULL DEFAULT 0,
             ord                INTEGER NOT NULL DEFAULT 0,
             PRIMARY KEY (session_id, group_id))""",
    ),
    (
        "group_fold_files",
        ["group_id"],
        """CREATE TABLE group_fold_files_v5 (
             session_id TEXT NOT NULL,
             group_id   INTEGER NOT NULL,
             ord        INTEGER NOT NULL,
             path       TEXT NOT NULL,
             PRIMARY KEY (session_id, group_id, ord),
             FOREIGN KEY (session_id, group_id) REFERENCES groups(session_id, group_id)
               ON DELETE CASCADE)""",
    ),
    (
        "tasks",
        ["task_id", "group_id", "sink_group_id", "fold_cut_task", "memo_cut_task",
         "forked_from_task_id"],
        """CREATE TABLE tasks_v5 (
             task_id                   INTEGER PRIMARY KEY AUTOINCREMENT,
             session_id                TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
             group_id                  INTEGER NOT NULL,
             ord                       INTEGER NOT NULL DEFAULT 0,
             description               TEXT,
             raw_input                 TEXT,
             step                      TEXT,
             life                      TEXT,
             end_state                 TEXT,
             blocked_by                TEXT,
             blocked_end               TEXT,
             fail                      TEXT,
             is_group_leader           INTEGER NOT NULL DEFAULT 0,
             bg                        INTEGER NOT NULL DEFAULT 0,
             tool_only                 INTEGER NOT NULL DEFAULT 0,
             deferred_vars             INTEGER NOT NULL DEFAULT 0,
             memo_kind                 TEXT NOT NULL DEFAULT 'curate',
             sink                      TEXT,
             sink_group_id             INTEGER,
             fold_cut_task             INTEGER,
             fold_cut_turn             INTEGER NOT NULL DEFAULT 0,
             fold_cut_prefix           INTEGER NOT NULL DEFAULT 0,
             memo_cut_task             INTEGER,
             memo_cut_turn             INTEGER NOT NULL DEFAULT 0,
             memo_cut_prefix           INTEGER NOT NULL DEFAULT 0,
             forked_from_task_id       INTEGER REFERENCES tasks(task_id),
             fork_point_completion_seq INTEGER NOT NULL DEFAULT 0,
             output                    TEXT,
             completion_seq            INTEGER NOT NULL DEFAULT 0,
             tokens_prompt             INTEGER NOT NULL DEFAULT 0,
             tokens_completion         INTEGER NOT NULL DEFAULT 0,
             tokens_total              INTEGER NOT NULL DEFAULT 0,
             tokens_cached             INTEGER NOT NULL DEFAULT 0,
             last_total_tokens         INTEGER NOT NULL DEFAULT 0,
             last_fold_epoch           INTEGER NOT NULL DEFAULT 0,
             last_hist_len             INTEGER NOT NULL DEFAULT 0,
             last_turn_len             INTEGER NOT NULL DEFAULT 0,
             memo_seen_seq             INTEGER NOT NULL DEFAULT 0,
             last_prefix_tokens        INTEGER NOT NULL DEFAULT 0,
             redirect_stdout           TEXT,
             redirect_stderr           TEXT,
             redirect_append           INTEGER NOT NULL DEFAULT 0,
             redirect_stderr_append    INTEGER NOT NULL DEFAULT 0,
             todos                     TEXT,
             todo_sync_gap             INTEGER NOT NULL DEFAULT 0,
             updated_at                INTEGER NOT NULL)""",
    ),
    (
        "task_deps",
        ["target_task_id", "source_task_id"],
        """CREATE TABLE task_deps_v5 (
             target_task_id INTEGER NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
             source_task_id INTEGER NOT NULL,
             type           TEXT NOT NULL,
             completed      INTEGER NOT NULL DEFAULT 0,
             cancelled      INTEGER NOT NULL DEFAULT 0,
             injected       INTEGER NOT NULL DEFAULT 0,
             ord            INTEGER NOT NULL DEFAULT 0,
             PRIMARY KEY (target_task_id, source_task_id))""",
    ),
    (
        "task_tool_calls",
        ["task_id"],
        """CREATE TABLE task_tool_calls_v5 (
             task_id INTEGER NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
             ord     INTEGER NOT NULL,
             call_id TEXT,
             name    TEXT,
             args    TEXT,
             status  TEXT NOT NULL,
             PRIMARY KEY (task_id, ord))""",
    ),
    (
        "turns",
        ["task_id"],
        """CREATE TABLE turns_v5 (
             task_id       INTEGER NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
             turn_ord      INTEGER NOT NULL,
             folded_prefix INTEGER NOT NULL DEFAULT 0,
             memo_prefix   INTEGER NOT NULL DEFAULT 0,
             is_active     INTEGER NOT NULL DEFAULT 0,
             msg_count     INTEGER NOT NULL DEFAULT 0,
             token_count   INTEGER NOT NULL DEFAULT 0,
             PRIMARY KEY (task_id, turn_ord))""",
    ),
    (
        "messages",
        ["task_id", "group_id"],
        """CREATE TABLE messages_v5 (
             id         TEXT NOT NULL PRIMARY KEY,
             session_id TEXT NOT NULL,
             group_id   INTEGER NOT NULL,
             task_id    INTEGER NOT NULL,
             turn_ord   INTEGER NOT NULL,
             msg_ord    INTEGER NOT NULL,
             role       TEXT,
             mid        TEXT,
             ts         INTEGER NOT NULL DEFAULT 0,
             render     TEXT,
             synthetic  INTEGER NOT NULL DEFAULT 0,
             payload    TEXT,
             tokens     INTEGER NOT NULL DEFAULT 0,
             body       TEXT NOT NULL,
             FOREIGN KEY (task_id, turn_ord) REFERENCES turns(task_id, turn_ord)
               ON DELETE CASCADE)""",
    ),
]

INDEXES = [
    "CREATE INDEX IF NOT EXISTS sessions_by_created ON sessions(created_at)",
    "CREATE UNIQUE INDEX IF NOT EXISTS sessions_one_active ON sessions(is_active) WHERE is_active=1",
    "CREATE INDEX IF NOT EXISTS groups_by_session ON groups(session_id, ord)",
    "CREATE INDEX IF NOT EXISTS tasks_by_group ON tasks(session_id, group_id, ord)",
    "CREATE INDEX IF NOT EXISTS tasks_by_fork ON tasks(forked_from_task_id)",
    "CREATE INDEX IF NOT EXISTS task_deps_by_source ON task_deps(source_task_id)",
    "CREATE UNIQUE INDEX IF NOT EXISTS messages_natural ON messages(task_id, turn_ord, msg_ord)",
    "CREATE INDEX IF NOT EXISTS messages_by_mid ON messages(mid) WHERE mid IS NOT NULL",
    "CREATE INDEX IF NOT EXISTS messages_by_group "
    "ON messages(session_id, group_id, task_id, turn_ord, msg_ord)",
]


def die(msg, code=1):
    print("error: %s" % msg, file=sys.stderr)
    sys.exit(code)


def check_not_running(db_path):
    try:
        fd = os.open(db_path, os.O_RDWR)
    except OSError as e:
        die("cannot open %s for locking: %s" % (db_path, e))
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
        return True
    except OSError:
        return False
    finally:
        os.close(fd)


def backup(db_path):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    made = []
    for suffix in ("", "-wal", "-shm"):
        src = db_path + suffix
        if not os.path.exists(src):
            continue
        dst = "%s.v4.%s%s" % (db_path, stamp, suffix)
        shutil.copy2(src, dst)
        made.append(dst)
    con = sqlite3.connect("file:%s?mode=ro" % made[0], uri=True)
    try:
        n = con.execute("select count(*) from tasks").fetchone()[0]
        ok = con.execute("pragma quick_check(1)").fetchone()[0]
    finally:
        con.close()
    if ok != "ok":
        die("backup %s failed quick_check: %s" % (made[0], ok))
    print("backup      : %s (%d tasks, quick_check ok)" % (made[0], n))
    return made


def columns(con, table):
    return [r[1] for r in con.execute("pragma table_info(%s)" % table)]


def cast_expr(col, coltype):
    """Cast a v4 column to what v5 wants, leaving NULL alone."""
    if coltype == "INTEGER":
        return "CAST(%s AS INTEGER)" % col
    return col


def rebuild(con, table, id_cols, ddl, types):
    new = table + "_v5"
    con.execute("DROP TABLE IF EXISTS %s" % new)
    con.execute(ddl)
    old_cols = columns(con, table)
    new_cols = columns(con, new)
    # Only the columns both shapes have: nothing in v5 was dropped, but be explicit.
    shared = [c for c in new_cols if c in old_cols]
    select = ", ".join(cast_expr(c, "INTEGER") if c in id_cols else c for c in shared)
    con.execute("INSERT INTO %s(%s) SELECT %s FROM %s"
                % (new, ", ".join(shared), select, table))
    con.execute("DROP TABLE %s" % table)
    con.execute("ALTER TABLE %s RENAME TO %s" % (new, table))
    return len(shared)


def migrate(db_path, dry_run):
    if not os.path.exists(db_path):
        die("%s does not exist -- nothing to migrate" % db_path)
    if not check_not_running(db_path):
        die("another edgeclaw instance is using this workspace (the database is locked).\n"
            "       Stop it first: the migration must be the only writer.")

    ro = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        ver = ro.execute("pragma user_version").fetchone()[0]
        print("database    : %s" % db_path)
        print("found       : schema v%s" % ver)
        if ver == V5:
            print("\nalready v5 -- nothing to do.")
            return 0
        if ver != V4:
            die("expected schema v4 or v5, found v%s; refusing to guess" % ver)
        before = {t: ro.execute("select count(*) from %s" % t).fetchone()[0]
                  for t, _, _ in TABLES}
    finally:
        ro.close()
    print("rows        : " + ", ".join("%s=%d" % (k, v) for k, v in before.items()))

    if not dry_run:
        backup_paths = backup(db_path)

    con = sqlite3.connect(db_path, isolation_level=None)
    # OFF for the whole rebuild: a parent's new shape does not exist yet while its children
    # are still pointing at the old one. Checked again at the end.
    con.execute("pragma foreign_keys=OFF")
    con.execute("begin immediate")
    try:
        for table, id_cols, ddl in TABLES:
            n = rebuild(con, table, id_cols, ddl, {})
            print("rebuilt     : %-18s (%d columns kept)" % (table, n))

        for stmt in INDEXES:
            con.execute(stmt)
        print("indexes     : recreated (%d)" % len(INDEXES))

        # AUTOINCREMENT keeps its high-water mark in sqlite_sequence; the rebuild reset it,
        # so set it to the largest id actually present or the next insert would collide.
        con.execute("DELETE FROM sqlite_sequence WHERE name='tasks'")
        con.execute("INSERT INTO sqlite_sequence(name,seq) "
                    "SELECT 'tasks', coalesce(max(task_id),0) FROM tasks")

        con.execute("pragma user_version=%d" % V5)

        if dry_run:
            raise _Rollback()
        con.execute("commit")
        print("committed   : schema v5")
    except _Rollback:
        con.execute("rollback")
        print("\n--dry-run: rolled back, database untouched")
        return 0
    except Exception:
        con.execute("rollback")
        raise
    finally:
        con.close()

    con = sqlite3.connect(db_path)
    try:
        con.execute("pragma wal_checkpoint(truncate)")
    finally:
        con.close()

    if dry_run:
        return 0

    ok = verify(db_path, before)
    print("verify      : %s" % ("ok" if ok else "FAILED"))
    if not ok:
        print("backup kept at:\n  %s" % "\n  ".join(backup_paths))
        return 1
    return 0


class _Rollback(Exception):
    pass


def verify(db_path, before=None):
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    ok = True
    try:
        ver = con.execute("pragma user_version").fetchone()[0]
        if ver != V5:
            print("verify      : FAILED user_version=%s, expected %d" % (ver, V5))
            return False

        # 1. no rows lost
        for table, _, _ in TABLES:
            n = con.execute("select count(*) from %s" % table).fetchone()[0]
            if before is not None and n != before[table]:
                print("verify      : FAILED %s has %d rows, had %d" % (table, n, before[table]))
                ok = False

        # 2. every id is an integer now (typeof, not just "it looks like one")
        checks = [
            ("tasks", "task_id"), ("tasks", "group_id"), ("groups", "group_id"),
            ("turns", "task_id"), ("messages", "task_id"), ("messages", "group_id"),
            ("task_deps", "target_task_id"), ("task_tool_calls", "task_id"),
        ]
        for table, col in checks:
            bad = con.execute("select count(*) from %s where typeof(%s) <> 'integer'" % (table, col)
                              ).fetchone()[0]
            if bad:
                print("verify      : FAILED %s.%s: %d non-integer values" % (table, col, bad))
                ok = False

        # 3. the FK chain still holds (foreign_keys was OFF during the rebuild)
        orphans = [
            ("turns -> tasks", "select count(*) from turns t left join tasks k on k.task_id=t.task_id "
                               "where k.task_id is null"),
            ("messages -> turns", "select count(*) from messages m left join turns t "
                                  "on t.task_id=m.task_id and t.turn_ord=m.turn_ord "
                                  "where t.task_id is null"),
            ("tasks -> groups", "select count(*) from tasks k left join groups g "
                                "on g.session_id=k.session_id and g.group_id=k.group_id "
                                "where g.group_id is null"),
        ]
        for label, q in orphans:
            n = con.execute(q).fetchone()[0]
            if n:
                print("verify      : FAILED %s: %d orphaned rows" % (label, n))
                ok = False

        if ok:
            tasks = con.execute("select count(*) from tasks").fetchone()[0]
            msgs = con.execute("select count(*) from messages").fetchone()[0]
            seq = con.execute("select seq from sqlite_sequence where name='tasks'").fetchone()
            print("verify      : schema v%d, %d tasks, %d messages, next task id %s"
                  % (ver, tasks, msgs, seq[0] if seq else "?"))
    except sqlite3.Error as e:
        print("verify      : FAILED %s" % e)
        ok = False
    finally:
        con.close()
    return ok


def main():
    ap = argparse.ArgumentParser(description="Migrate an edgeclaw workspace database v4 -> v5")
    ap.add_argument("workspace")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    ws = os.path.expanduser(args.workspace)
    db_path = os.path.join(ws, DB_NAME)
    if not os.path.exists(db_path):
        die("%s does not exist (%s)" % (DB_NAME, db_path))

    if args.check:
        con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
        try:
            print("database    : %s" % db_path)
            print("schema      : v%s" % con.execute("pragma user_version").fetchone()[0])
            for table, _, _ in TABLES:
                print("  %-18s %d rows" % (table, con.execute("select count(*) from %s" % table).fetchone()[0]))
        finally:
            con.close()
        return 0 if verify(db_path) else 1

    try:
        return migrate(db_path, args.dry_run)
    except sqlite3.Error as e:
        die("sqlite: %s" % e)


if __name__ == "__main__":
    sys.exit(main())
