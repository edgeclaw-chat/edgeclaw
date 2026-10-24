#!/usr/bin/env python3
"""Migrate an edgeclaw workspace database from schema v3 to v4.

WHAT CHANGED IN v4
    sessions.is_active -- which session the UI is on. It used to live only in
    <workspace>/sessions.json, which is why the session LIST also lived there; the index
    was the only place "where am I" could be written down. As a column it survives a kill
    like everything else, and the list became a plain query.

    Two indexes come with it:
      sessions_one_active  UNIQUE(is_active) WHERE is_active=1
                           -- "at most one active" is enforced, not assumed
      sessions_by_created  the order the list is read in

    The change is additive (one column, two indexes), so nothing is rebuilt.

THE ONE DATA DECISION
    The old active session is in sessions.json as `active_session_id`. This reads it and
    marks that row. Without it the first start would have to guess (it falls back to the
    newest session, which is usually right but not always). The file itself is left alone:
    removing it is the operator's call, and this script does not need it again.

SAFETY
    * Refuses to run while an edgeclaw process holds the workspace lock.
    * Backs up the database (-wal/-shm too) and verifies the backup before writing.
    * One transaction: a failure leaves the file exactly as it was.
    * Idempotent: a v4 file is reported and left alone.
    * --dry-run reports and rolls back.

USAGE
    python3 tools/migrate_edgeclaw_db_v3_to_v4.py <workspace> [--dry-run|--check]
"""

import argparse
import fcntl
import glob
import json
import os
import shutil
import sqlite3
import sys
import time

DB_NAME = "edgeclaw.db"
V3 = 3
V4 = 4


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
        dst = "%s.v3.%s%s" % (db_path, stamp, suffix)
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


def old_active_session(ws):
    """active_session_id from sessions.json, when there is one."""
    path = os.path.join(ws, "sessions.json")
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError):
        return None
    sid = doc.get("active_session_id")
    return sid if isinstance(sid, str) and sid else None


def columns(con, table):
    return {r[1] for r in con.execute("pragma table_info(%s)" % table)}


def report(con):
    ver = con.execute("pragma user_version").fetchone()[0]
    print("schema      : v%s" % ver)
    for table in ("sessions", "groups", "tasks", "turns", "messages"):
        try:
            n = con.execute("select count(*) from %s" % table).fetchone()[0]
            print("  %-9s : %d rows" % (table, n))
        except sqlite3.Error as e:
            print("  %-9s : ? (%s)" % (table, e))
    return ver


def migrate(db_path, ws, dry_run):
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
        if ver == V4:
            print("\nalready v4 -- nothing to do.")
            return 0
        if ver != V3:
            die("expected schema v3 or v4, found v%s; refusing to guess" % ver)
    finally:
        ro.close()

    wanted = old_active_session(ws)
    print("active hint : %s" % (wanted or "(none -- will pick the newest session)"))

    if not dry_run:
        backup_paths = backup(db_path)

    con = sqlite3.connect(db_path, isolation_level=None)
    con.execute("pragma foreign_keys=OFF")
    con.execute("begin immediate")
    try:
        if "is_active" not in columns(con, "sessions"):
            con.execute("alter table sessions add column is_active integer not null default 0")
            print("added       : sessions.is_active")
        else:
            print("added       : (already present)")

        # Two indexes, no table rewrite. NOT executescript(): it issues an implicit COMMIT,
        # which would end the transaction (and make --dry-run unable to roll back).
        con.execute("CREATE UNIQUE INDEX IF NOT EXISTS sessions_one_active "
                    "ON sessions(is_active) WHERE is_active=1")
        con.execute("CREATE INDEX IF NOT EXISTS sessions_by_created ON sessions(created_at)")
        print("indexed     : sessions_one_active, sessions_by_created")

        # Mark the session the old index file named. The unique index makes a wrong pick
        # loud rather than silent, so clear first.
        con.execute("update sessions set is_active=0")
        if wanted:
            cur = con.execute("update sessions set is_active=1 where session_id=?", (wanted,))
            if cur.rowcount == 0:
                print("warn        : %s is not in the database -- falling back to newest" % wanted)
                wanted = None
        if not wanted:
            row = con.execute("select session_id from sessions order by created_at desc, session_id desc "
                              "limit 1").fetchone()
            if row:
                con.execute("update sessions set is_active=1 where session_id=?", (row[0],))
                print("active      : %s (newest)" % row[0])
        else:
            print("active      : %s (from sessions.json)" % wanted)

        con.execute("pragma user_version=%d" % V4)

        if dry_run:
            raise _Rollback()
        con.execute("commit")
        print("committed   : schema v4")
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

    if not dry_run:
        ok = verify(db_path)
        print("verify      : %s" % ("ok" if ok else "FAILED"))
        if not ok:
            print("backup kept at:\n  %s" % "\n  ".join(backup_paths))
            return 1
    return 0


class _Rollback(Exception):
    pass


def verify(db_path):
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    ok = True
    try:
        ver = con.execute("pragma user_version").fetchone()[0]
        if ver != V4:
            print("verify      : FAILED user_version=%s, expected %d" % (ver, V4))
            return False
        cols = columns(con, "sessions")
        if "is_active" not in cols:
            print("verify      : FAILED sessions.is_active missing")
            ok = False
        active = con.execute("select count(*) from sessions where is_active=1").fetchone()[0]
        total = con.execute("select count(*) from sessions").fetchone()[0]
        if total > 0 and active != 1:
            print("verify      : FAILED %d active sessions (expected exactly 1 of %d)" % (active, total))
            ok = False
        idx = {r[0] for r in con.execute(
            "select name from sqlite_master where type='index' and tbl_name='sessions'")}
        for need in ("sessions_one_active", "sessions_by_created"):
            if need not in idx:
                print("verify      : FAILED index %s missing" % need)
                ok = False
        if ok:
            who = con.execute("select session_id from sessions where is_active=1").fetchone()
            print("verify      : schema v%d, %d sessions, active=%s"
                  % (ver, total, who[0] if who else "(none)"))
    except sqlite3.Error as e:
        print("verify      : FAILED %s" % e)
        ok = False
    finally:
        con.close()
    return ok


def main():
    ap = argparse.ArgumentParser(description="Migrate an edgeclaw workspace database v3 -> v4")
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
            report(con)
        finally:
            con.close()
        return 0 if verify(db_path) else 1

    try:
        return migrate(db_path, ws, args.dry_run)
    except sqlite3.Error as e:
        die("sqlite: %s" % e)


if __name__ == "__main__":
    sys.exit(main())
