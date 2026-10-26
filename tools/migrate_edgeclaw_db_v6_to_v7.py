#!/usr/bin/env python3
"""Migrate an edgeclaw workspace database from schema v6 to v7.

WHAT CHANGED IN v7
    group_fold_files gained a `kind` column, and its primary key went from
    (session_id, group_id, ord) to (session_id, group_id, kind, ord).

    A group's tracked-file state is TWO lists -- files it read and files it modified -- and
    the table had room for one flat sequence of paths. Combined with a flush that tested
    cJSON_IsArray on a {"modified":[...],"read":[...]} object, the table was never written on
    any workspace since the store landed, and the load (which built a flat array) could not
    have read it back either. Both sides now go through edc_fold_files_*(), so the shape
    lives in one place.

NO DATA TO CARRY
    Every real workspace has zero rows here, because nothing ever wrote one. This script
    still copies whatever it finds, labelled 'read': a row's old shape said "a path this
    group touched" and nothing more, so 'read' is the honest label and 'modified' would
    invent information.

SAFETY
    * Refuses to run while an edgeclaw process holds the workspace lock.
    * Backs up the database (-wal/-shm too) and verifies the backup before writing.
    * One transaction: a failure leaves the file exactly as it was.
    * --dry-run reports and rolls back; --check reports only.
    * Idempotent: a v7 file is reported and left alone.
    * Verifies afterwards: row counts unchanged, every row has a known kind, and no orphaned
      group references.

USAGE
    python3 tools/migrate_edgeclaw_db_v6_to_v7.py <workspace> [--dry-run|--check]

NOTE ON TEMPORARY SPACE
    A rebuild spills to TMPDIR. If TMPDIR is a small tmpfs the run dies with "database disk
    image is malformed" or a spurious NOT NULL failure -- both are ENOSPC in disguise. Point
    SQLite at the same filesystem as the database:

        SQLITE_TMPDIR=$HOME/.cache python3 tools/migrate_edgeclaw_db_v6_to_v7.py <workspace>
"""

import argparse
import fcntl
import os
import shutil
import sqlite3
import sys
import time

DB_NAME = "edgeclaw.db"
V6 = 6
V7 = 7

NEW_DDL = """CREATE TABLE group_fold_files_v7 (
     session_id INTEGER NOT NULL,
     group_id   INTEGER NOT NULL,
     kind       TEXT NOT NULL,
     ord        INTEGER NOT NULL,
     path       TEXT NOT NULL,
     PRIMARY KEY (session_id, group_id, kind, ord),
     FOREIGN KEY (session_id, group_id) REFERENCES groups(session_id, group_id)
       ON DELETE CASCADE)"""


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
        dst = "%s.v6.%s%s" % (db_path, stamp, suffix)
        shutil.copy2(src, dst)
        made.append(dst)
    con = sqlite3.connect("file:%s?mode=ro" % made[0], uri=True)
    try:
        n = con.execute("select count(*) from group_fold_files").fetchone()[0]
        ok = con.execute("pragma quick_check(1)").fetchone()[0]
    finally:
        con.close()
    if ok != "ok":
        die("backup %s failed quick_check: %s" % (made[0], ok))
    print("backup      : %s (%d fold-file rows, quick_check ok)" % (made[0], n))
    return made


def columns(con, table):
    return [r[1] for r in con.execute("pragma table_info(%s)" % table)]


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
        if ver == V7:
            print("\nalready v7 -- nothing to do.")
            return 0
        if ver != V6:
            die("expected schema v6 or v7, found v%s; refusing to guess" % ver)
        before = {t: ro.execute("select count(*) from %s" % t).fetchone()[0]
                  for t in ("sessions", "groups", "group_fold_files")}
        old_cols = columns(ro, "group_fold_files")
    finally:
        ro.close()
    print("rows        : " + ", ".join("%s=%d" % (k, v) for k, v in before.items()))
    print("old columns : %s" % ", ".join(old_cols))

    if not dry_run:
        backup_paths = backup(db_path)

    con = sqlite3.connect(db_path, isolation_level=None)
    con.execute("pragma foreign_keys=OFF")
    con.execute("begin immediate")
    try:
        con.execute("DROP TABLE IF EXISTS group_fold_files_v7")
        con.execute(NEW_DDL)

        # A v6 row says only "this group touched this path"; it cannot say which list. 'read'
        # is the honest default (see the module note in the header).
        rows = con.execute("select session_id,group_id,ord,path from group_fold_files "
                          "order by session_id,group_id,ord").fetchall()
        for sid, gid, ordv, path in rows:
            con.execute("INSERT INTO group_fold_files_v7(session_id,group_id,kind,ord,path) "
                        "VALUES(?,?,'read',?,?)", (sid, gid, ordv, path))
        print("rebuilt     : group_fold_files   (%d rows re-labelled 'read')" % len(rows))

        con.execute("DROP TABLE group_fold_files")
        con.execute("ALTER TABLE group_fold_files_v7 RENAME TO group_fold_files")
        con.execute("pragma user_version=%d" % V7)

        if dry_run:
            raise _Rollback()
        con.execute("commit")
        print("committed   : schema v7")
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
        if ver != V7:
            print("verify      : FAILED user_version=%s, expected %d" % (ver, V7))
            return False

        if before is not None:
            for table in ("sessions", "groups", "group_fold_files"):
                n = con.execute("select count(*) from %s" % table).fetchone()[0]
                if n != before[table]:
                    print("verify      : FAILED %s has %d rows, had %d" % (table, n, before[table]))
                    ok = False

        cols = columns(con, "group_fold_files")
        if "kind" not in cols:
            print("verify      : FAILED group_fold_files has no kind column")
            ok = False
        if cols[:4] != ["session_id", "group_id", "kind", "ord"]:
            print("verify      : FAILED key order is %s" % cols[:4])
            ok = False

        bad = con.execute("select count(*) from group_fold_files "
                          "where kind not in ('read','modified')").fetchone()[0]
        if bad:
            print("verify      : FAILED %d rows with an unknown kind" % bad)
            ok = False

        orphans = con.execute("select count(*) from group_fold_files f "
                              "left join groups g on g.session_id=f.session_id and g.group_id=f.group_id "
                              "where g.group_id is null").fetchone()[0]
        if orphans:
            print("verify      : FAILED %d rows point at no group" % orphans)
            ok = False

        if ok:
            n = con.execute("select count(*) from group_fold_files").fetchone()[0]
            print("verify      : schema v%d, %d fold-file rows, both kinds legal" % (ver, n))
    except sqlite3.Error as e:
        print("verify      : FAILED %s" % e)
        ok = False
    finally:
        con.close()
    return ok


def main():
    ap = argparse.ArgumentParser(description="Migrate an edgeclaw workspace database v6 -> v7")
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
            print("  group_fold_files %d rows" %
                  con.execute("select count(*) from group_fold_files").fetchone()[0])
        finally:
            con.close()
        return 0 if verify(db_path) else 1

    try:
        return migrate(db_path, args.dry_run)
    except sqlite3.Error as e:
        die("sqlite: %s" % e)


if __name__ == "__main__":
    sys.exit(main())
