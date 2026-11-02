#!/usr/bin/env python3
"""Migrate an edgeclaw workspace database from schema v7 to v8.

WHAT CHANGED IN v8
    Two session-memory fields became explicit state. Both were previously encoded in
    something else, which is exactly the kind of cleverness that rots:

    1. tasks.memo_baseline_done
       Whether a task has received the session-memory CATALOGUE (the "what exists" card,
       id + title). It used to be inferred from `memo_seen_seq == 0`, which overloaded one
       watermark with two unrelated facts: "this task never saw the index" and "this task's
       change cursor sits at the beginning of the feed". It could not express the case that
       matters most -- the catalogue was delivered while the memory was still empty, so the
       task has seen an index (of nothing) and must not be treated as un-baselined forever.

    2. memo_events.changed
       An EDC_MEMO_F_* bit mask recording WHICH fields a change touched (title / summary /
       detail / retired). It was in-memory only, so a restart silently turned every past
       change into "unknown" and the change notices fell back to re-printing whole
       summaries -- the expensive rendering the mask exists to avoid.

HOW THE DATA IS BACKFILLED
    memo_baseline_done = (memo_seen_seq > 0)

    A task that had already consumed events has necessarily been through the index: under
    v7, every notice delivery advanced `memo_seen_seq`, and the very first request of a
    task carried the backfill notice. Tasks sitting at 0 never saw an index and get the
    catalogue on their next request -- which is the correct, cheap behaviour.

    changed = 0

    NOT a guess. 0 is the documented "unknown" value: the notice renders such an event with
    its content, exactly as v7 did. Reconstructing the mask is impossible -- it describes a
    transition whose previous value no longer exists anywhere.

SAFETY
    * Refuses to run while an edgeclaw process holds the workspace lock.
    * Backs up the database (-wal/-shm too) and verifies the backup before writing.
    * One transaction: a failure leaves the file exactly as it was.
    * --dry-run reports and rolls back; --check reports only.
    * Idempotent: a v8 file is reported and left alone.
    * Verifies afterwards: row counts unchanged, the new columns exist, every
      memo_baseline_done is 0/1, and the backfill invariant holds (no task is marked
      un-baselined while its watermark is past the start).

USAGE
    python3 tools/migrate_edgeclaw_db_v7_to_v8.py <workspace> [--dry-run|--check]

NOTE ON TEMPORARY SPACE
    SQLite spills to TMPDIR. If TMPDIR is a small tmpfs the run can die with a spurious
    "database disk image is malformed" or NOT NULL failure -- both are ENOSPC in disguise.
    Point SQLite at the same filesystem as the database:

        SQLITE_TMPDIR=$HOME/.cache python3 tools/migrate_edgeclaw_db_v7_to_v8.py <workspace>
"""

import argparse
import fcntl
import os
import shutil
import sqlite3
import sys
import time

DB_NAME = "edgeclaw.db"
V7 = 7
V8 = 8

# ALTER TABLE ADD COLUMN with a constant default is O(1) in SQLite and needs no table
# rebuild, so the migration stays a couple of statements instead of a copy of every row.
ADD_COLUMNS = (
    "ALTER TABLE tasks ADD COLUMN memo_baseline_done INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE memo_events ADD COLUMN changed INTEGER NOT NULL DEFAULT 0",
)

# See "HOW THE DATA IS BACKFILLED": a task past the start of the feed was necessarily sent
# the index at some point, so it is already baselined.
BACKFILL_BASELINE = (
    "UPDATE tasks SET memo_baseline_done = CASE WHEN memo_seen_seq > 0 THEN 1 ELSE 0 END"
)


def die(msg, code=1):
    print("error: %s" % msg, file=sys.stderr)
    sys.exit(code)


def workspace_free(db_path):
    if not os.path.exists(db_path):
        return True
    try:
        fd = os.open(db_path, os.O_RDWR)
    except OSError:
        return False
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
        dst = "%s.v7.%s%s" % (db_path, stamp, suffix)
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
    print("backup      : %s (%d task rows, quick_check ok)" % (made[0], n))
    return made


def columns(con, table):
    return [r[1] for r in con.execute("pragma table_info(%s)" % table)]


class _Rollback(Exception):
    pass


def migrate(db_path, dry_run):
    if not os.path.exists(db_path):
        die("%s does not exist -- nothing to migrate" % db_path)
    if not workspace_free(db_path):
        die("another edgeclaw instance is using this workspace (the database is locked).\n"
            "       Stop it first: the migration must be the only writer.")

    ro = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        ver = ro.execute("pragma user_version").fetchone()[0]
        print("database    : %s" % db_path)
        print("found       : schema v%s" % ver)
        if ver == V8:
            print("\nalready v8 -- nothing to do.")
            return 0
        if ver != V7:
            die("expected schema v7 or v8, found v%s; refusing to guess" % ver)
        before = {t: ro.execute("select count(*) from %s" % t).fetchone()[0]
                  for t in ("sessions", "groups", "tasks", "memo_entries", "memo_events")}
        seen_nonzero = ro.execute("select count(*) from tasks where memo_seen_seq > 0").fetchone()[0]
    finally:
        ro.close()
    print("rows        : " + ", ".join("%s=%d" % (k, v) for k, v in before.items()))
    print("backfill    : %d of %d tasks will be marked memo_baseline_done=1 (memo_seen_seq > 0)"
          % (seen_nonzero, before["tasks"]))
    print("changed     : left at 0 (\"unknown\") -- old events keep the content rendering")

    if not dry_run:
        backup_paths = backup(db_path)

    con = sqlite3.connect(db_path, isolation_level=None)
    con.execute("pragma foreign_keys=OFF")
    con.execute("begin immediate")
    try:
        for stmt in ADD_COLUMNS:
            con.execute(stmt)
        con.execute(BACKFILL_BASELINE)
        con.execute("pragma user_version=%d" % V8)
        print("added       : tasks.memo_baseline_done, memo_events.changed")

        if dry_run:
            raise _Rollback()
        con.execute("commit")
        print("committed   : schema v8")
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


def verify(db_path, before=None):
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    ok = True
    try:
        ver = con.execute("pragma user_version").fetchone()[0]
        if ver != V8:
            print("verify      : FAILED user_version=%s, expected %d" % (ver, V8))
            return False

        if before is not None:
            for table in ("sessions", "groups", "tasks", "memo_entries", "memo_events"):
                n = con.execute("select count(*) from %s" % table).fetchone()[0]
                if n != before[table]:
                    print("verify      : FAILED %s has %d rows, had %d" % (table, n, before[table]))
                    ok = False

        if "memo_baseline_done" not in columns(con, "tasks"):
            print("verify      : FAILED tasks has no memo_baseline_done column")
            ok = False
        if "changed" not in columns(con, "memo_events"):
            print("verify      : FAILED memo_events has no changed column")
            ok = False

        # The new columns are flags/masks, not free-form values: anything else means the
        # backfill or the DDL did something unexpected.
        bad = con.execute("select count(*) from tasks "
                          "where memo_baseline_done not in (0,1)").fetchone()[0]
        if bad:
            print("verify      : FAILED %d tasks have a non-boolean memo_baseline_done" % bad)
            ok = False

        # The backfill invariant: nothing may sit un-baselined with a watermark past the
        # start of the feed, or it would be sent an index it has already seen.
        wrong = con.execute("select count(*) from tasks "
                            "where memo_seen_seq > 0 and memo_baseline_done = 0").fetchone()[0]
        if wrong:
            print("verify      : FAILED %d tasks are un-baselined but have memo_seen_seq > 0" % wrong)
            ok = False

        if ok:
            n = con.execute("select count(*) from tasks where memo_baseline_done = 1").fetchone()[0]
            m = con.execute("select count(*) from memo_events").fetchone()[0]
            print("verify      : schema v%d, %d baselined tasks, %d events (mask=0 => unknown)" % (ver, n, m))
    except sqlite3.Error as e:
        print("verify      : FAILED %s" % e)
        ok = False
    finally:
        con.close()
    return ok


def main():
    ap = argparse.ArgumentParser(description="Migrate an edgeclaw workspace database v7 -> v8")
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
            print("  tasks           %d rows" % con.execute("select count(*) from tasks").fetchone()[0])
            print("  memo_events     %d rows" % con.execute("select count(*) from memo_events").fetchone()[0])
        finally:
            con.close()
        return 0 if verify(db_path) else 1

    try:
        return migrate(db_path, args.dry_run)
    except sqlite3.Error as e:
        die("sqlite: %s" % e)


if __name__ == "__main__":
    sys.exit(main())
