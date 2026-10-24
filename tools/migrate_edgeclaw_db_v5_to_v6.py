#!/usr/bin/env python3
"""Migrate an edgeclaw workspace database from schema v5 to v6.

WHAT CHANGED IN v6
    session_id became an INTEGER PRIMARY KEY AUTOINCREMENT, assigned by the INSERT -- the
    same change v5 made for task_id, for the same reason. It used to be the TEXT "s<N>" from
    a counter whose high-water mark lived in <workspace>/.session_seq; a recycled id would
    land a new session on a removed one's directory and replay its chat.jsonl, which is
    exactly what AUTOINCREMENT prevents.

    Every table that references a session changes type with it: groups, group_fold_files,
    tasks, messages, memo_entries, memo_events.

WHY THE CAST IS NOT BARE
    The existing values are not decimal -- they are "s7", "s13", "s15". `CAST('s7' AS
    INTEGER)` is 0, which would collapse every session onto one id and take the whole
    database with it. The prefix is stripped first:

        CAST(substr(session_id, 2) AS INTEGER)          -- "s7"      -> 7
        CAST(substr(session_id, 6) AS INTEGER)          -- "sess-7"  -> 7
        <assigned in order>                             -- anything else

    A value that is not one of those shapes gets a fresh id in creation order rather than
    silently becoming 0 (a 0 would collide with every other unparseable row, and sessions
    .session_id has a UNIQUE constraint, so the insert would fail loudly -- better to be
    deliberate).

THE DIRECTORY IS NOT RENAMED
    <workspace>/s7/ keeps its name. Those directories hold live chat.jsonl transcripts the
    TUI reads and appends; renaming them would be a data migration in exchange for a
    cosmetic change. The mapping old-id -> new-id is stable here (s7 -> 7), so the
    directories line up without being touched.

SAFETY
    * Refuses to run while an edgeclaw process holds the workspace lock.
    * Backs up the database (-wal/-shm too) and verifies the backup before writing.
    * One transaction: a failure leaves the file exactly as it was.
    * --dry-run reports and rolls back.
    * Idempotent: a v6 file is reported and left alone.
    * Verifies afterwards: row counts unchanged, every session_id is a positive integer, no
      orphaned child rows, and sqlite_sequence is set so the next insert cannot collide.

USAGE
    python3 tools/migrate_edgeclaw_db_v5_to_v6.py <workspace> [--dry-run|--check]

NOTE ON TEMPORARY SPACE
    Rebuilding `messages` copies every row inside one transaction, and SQLite spills the
    intermediate state to temp files. If TMPDIR is a small tmpfs the run dies half way with
    "database disk image is malformed" or a spurious NOT NULL failure -- both are ENOSPC in
    disguise, not corruption. Point SQLite at the same filesystem as the database:

        SQLITE_TMPDIR=$HOME/.cache python3 tools/migrate_edgeclaw_db_v5_to_v6.py <workspace>
"""

import argparse
import fcntl
import os
import shutil
import sqlite3
import sys
import time

DB_NAME = "edgeclaw.db"
V5 = 5
V6 = 6

# Parent-first. (table, new DDL). The session_id column is rebuilt from the mapping table;
# every other column is copied as-is.
TABLES = [
    ("sessions", """CREATE TABLE sessions_v6 (
         session_id            INTEGER PRIMARY KEY AUTOINCREMENT,
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
         updated_at            INTEGER NOT NULL)"""),
    ("groups", """CREATE TABLE groups_v6 (
         session_id         INTEGER NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
         group_id           INTEGER NOT NULL,
         is_orphaned        INTEGER NOT NULL DEFAULT 0,
         kind               TEXT NOT NULL DEFAULT 'user',
         memory             TEXT,
         fold_epoch         INTEGER NOT NULL DEFAULT 0,
         memory_created_at  INTEGER NOT NULL DEFAULT 0,
         memory_updated_at  INTEGER NOT NULL DEFAULT 0,
         memory_covers_to   INTEGER NOT NULL DEFAULT 0,
         ord                INTEGER NOT NULL DEFAULT 0,
         PRIMARY KEY (session_id, group_id))"""),
    ("group_fold_files", """CREATE TABLE group_fold_files_v6 (
         session_id INTEGER NOT NULL,
         group_id   INTEGER NOT NULL,
         ord        INTEGER NOT NULL,
         path       TEXT NOT NULL,
         PRIMARY KEY (session_id, group_id, ord),
         FOREIGN KEY (session_id, group_id) REFERENCES groups(session_id, group_id)
           ON DELETE CASCADE)"""),
    ("tasks", """CREATE TABLE tasks_v6 (
         task_id                   INTEGER PRIMARY KEY AUTOINCREMENT,
         session_id                INTEGER NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
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
         updated_at                INTEGER NOT NULL)"""),
    ("messages", """CREATE TABLE messages_v6 (
         id         TEXT NOT NULL PRIMARY KEY,
         session_id INTEGER NOT NULL,
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
           ON DELETE CASCADE)"""),
    ("memo_entries", """CREATE TABLE memo_entries_v6 (
         session_id  INTEGER NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
         entry_id    TEXT NOT NULL,
         title       TEXT,
         summary     TEXT,
         detail      TEXT,
         sources     TEXT,
         proposed_by TEXT,
         created_seq INTEGER NOT NULL DEFAULT 0,
         updated_seq INTEGER NOT NULL DEFAULT 0,
         created_at  INTEGER NOT NULL DEFAULT 0,
         updated_at  INTEGER NOT NULL DEFAULT 0,
         superseded  INTEGER NOT NULL DEFAULT 0,
         PRIMARY KEY (session_id, entry_id))"""),
    ("memo_events", """CREATE TABLE memo_events_v6 (
         session_id INTEGER NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
         seq        INTEGER NOT NULL,
         ts         INTEGER NOT NULL DEFAULT 0,
         entry_id   TEXT,
         kind       TEXT,
         title      TEXT,
         summary    TEXT,
         PRIMARY KEY (session_id, seq))"""),
]

INDEXES = [
    "CREATE UNIQUE INDEX IF NOT EXISTS sessions_one_active ON sessions(is_active) WHERE is_active=1",
    "CREATE INDEX IF NOT EXISTS sessions_by_created ON sessions(created_at)",
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
        dst = "%s.v5.%s%s" % (db_path, stamp, suffix)
        shutil.copy2(src, dst)
        made.append(dst)
    con = sqlite3.connect("file:%s?mode=ro" % made[0], uri=True)
    try:
        n = con.execute("select count(*) from sessions").fetchone()[0]
        ok = con.execute("pragma quick_check(1)").fetchone()[0]
    finally:
        con.close()
    if ok != "ok":
        die("backup %s failed quick_check: %s" % (made[0], ok))
    print("backup      : %s (%d sessions, quick_check ok)" % (made[0], n))
    return made


def parse_old_id(old):
    """'s7' -> 7, 'sess-7' -> 7, '7' -> 7, anything else -> None."""
    if not old:
        return None
    s = str(old)
    if s.startswith("sess-"):
        s = s[5:]
    elif s.startswith("s") and len(s) > 1 and s[1].isdigit():
        s = s[1:]
    if s.isdigit():
        v = int(s)
        return v if v > 0 else None
    return None


def build_mapping(con):
    """old session_id -> new integer id, preserving numeric order."""
    old_ids = [r[0] for r in con.execute("select session_id from sessions")]
    mapping = {}
    taken = set()

    # Pass 1: the ones whose text already carries a number.
    numeric = []
    for old in old_ids:
        v = parse_old_id(old)
        if v is not None and v not in taken:
            mapping[old] = v
            taken.add(v)
        else:
            numeric.append(old)

    # Pass 2: anything else gets the next free number, in creation order.
    nxt = max(taken) + 1 if taken else 1
    for old in numeric:
        while nxt in taken:
            nxt += 1
        mapping[old] = nxt
        taken.add(nxt)
        print("              reassigned %r -> %d (not an s<N> value)" % (old, nxt))
    return mapping


def columns(con, table):
    return [r[1] for r in con.execute("pragma table_info(%s)" % table)]


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
        if ver == V6:
            print("\nalready v6 -- nothing to do.")
            return 0
        if ver != V5:
            die("expected schema v5 or v6, found v%s; refusing to guess" % ver)
        before = {t: ro.execute("select count(*) from %s" % t).fetchone()[0] for t, _ in TABLES}
    finally:
        ro.close()
    print("rows        : " + ", ".join("%s=%d" % (k, v) for k, v in before.items()))

    if not dry_run:
        backup_paths = backup(db_path)

    con = sqlite3.connect(db_path, isolation_level=None)
    con.execute("pragma foreign_keys=OFF")
    con.execute("begin immediate")
    try:
        mapping = build_mapping(con)
        print("sessions    : " + ", ".join("%s->%d" % (k, v) for k, v in
                                          sorted(mapping.items(), key=lambda kv: kv[1])))

        for table, ddl in TABLES:
            new = table + "_v6"
            con.execute("DROP TABLE IF EXISTS %s" % new)
            con.execute(ddl)
            old_cols = columns(con, table)
            new_cols = columns(con, new)
            shared = [c for c in new_cols if c in old_cols]
            select = ", ".join("?" if c == "session_id" else c for c in shared)

            if "session_id" in shared:
                # Row by row: the new session id comes from the mapping, not from SQL.
                rows = con.execute("select %s from %s" % (", ".join(shared), table)).fetchall()
                si = shared.index("session_id")
                fixed = []
                for r in rows:
                    r = list(r)
                    r[si] = mapping.get(r[si], 0)
                    fixed.append(r)
                con.executemany("INSERT INTO %s(%s) VALUES(%s)"
                                % (new, ", ".join(shared), ", ".join("?" * len(shared))), fixed)
                print("rebuilt     : %-18s (%d rows, session_id remapped)" % (table, len(fixed)))
            else:
                con.execute("INSERT INTO %s(%s) SELECT %s FROM %s"
                            % (new, ", ".join(shared), ", ".join(shared), table))
                print("rebuilt     : %-18s (%d columns copied)" % (table, len(shared)))

            con.execute("DROP TABLE %s" % table)
            con.execute("ALTER TABLE %s RENAME TO %s" % (new, table))

        for stmt in INDEXES:
            con.execute(stmt)
        print("indexes     : recreated (%d)" % len(INDEXES))

        # AUTOINCREMENT keeps its high-water mark in sqlite_sequence; the rebuild reset it.
        for tbl, col in (("sessions", "session_id"), ("tasks", "task_id")):
            con.execute("DELETE FROM sqlite_sequence WHERE name='%s'" % tbl)
            con.execute("INSERT INTO sqlite_sequence(name,seq) "
                        "SELECT '%s', coalesce(max(%s),0) FROM %s" % (tbl, col, tbl))

        con.execute("pragma user_version=%d" % V6)

        if dry_run:
            raise _Rollback()
        con.execute("commit")
        print("committed   : schema v6")
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
        if ver != V6:
            print("verify      : FAILED user_version=%s, expected %d" % (ver, V6))
            return False

        for table, _ in TABLES:
            n = con.execute("select count(*) from %s" % table).fetchone()[0]
            if before is not None and n != before[table]:
                print("verify      : FAILED %s has %d rows, had %d" % (table, n, before[table]))
                ok = False

        for table, _ in TABLES:
            bad = con.execute("select count(*) from %s where typeof(session_id) <> 'integer'"
                              % table).fetchone()[0]
            if bad:
                print("verify      : FAILED %s.session_id: %d non-integer values" % (table, bad))
                ok = False
            zero = con.execute("select count(*) from %s where session_id <= 0" % table).fetchone()[0]
            if zero:
                print("verify      : FAILED %s.session_id: %d values <= 0" % (table, zero))
                ok = False

        orphans = [
            ("groups -> sessions", "select count(*) from groups g left join sessions s "
                                   "on s.session_id=g.session_id where s.session_id is null"),
            ("tasks -> sessions", "select count(*) from tasks t left join sessions s "
                                  "on s.session_id=t.session_id where s.session_id is null"),
            ("messages -> sessions", "select count(*) from messages m left join sessions s "
                                     "on s.session_id=m.session_id where s.session_id is null"),
            ("messages -> turns", "select count(*) from messages m left join turns t "
                                  "on t.task_id=m.task_id and t.turn_ord=m.turn_ord "
                                  "where t.task_id is null"),
        ]
        for label, q in orphans:
            n = con.execute(q).fetchone()[0]
            if n:
                print("verify      : FAILED %s: %d orphaned rows" % (label, n))
                ok = False

        if ok:
            s = con.execute("select count(*) from sessions").fetchone()[0]
            t = con.execute("select count(*) from tasks").fetchone()[0]
            m = con.execute("select count(*) from messages").fetchone()[0]
            seq = con.execute("select seq from sqlite_sequence where name='sessions'").fetchone()
            print("verify      : schema v%d, %d sessions, %d tasks, %d messages, next session id %s"
                  % (ver, s, t, m, seq[0] if seq else "?"))
    except sqlite3.Error as e:
        print("verify      : FAILED %s" % e)
        ok = False
    finally:
        con.close()
    return ok


def main():
    ap = argparse.ArgumentParser(description="Migrate an edgeclaw workspace database v5 -> v6")
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
            for table, _ in TABLES:
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
