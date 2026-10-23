#!/usr/bin/env python3
"""Migrate an edgeclaw workspace database from schema v1 to v2.

WHY THIS EXISTS
    edc_state_store.h declares "no migration path, by design": a database written by a
    different EDC_DB_VERSION is refused with an instruction to delete it. That rule is
    about not carrying compatibility code -- it is not a licence to throw away somebody's
    real conversation history. v2 changes are ADDITIVE ONLY (three new columns), so the
    data can be carried across with one ALTER TABLE and a backfill, which is what this
    script does. It is a one-shot local repair, not a compatibility layer.

WHAT CHANGED IN v2
    turns.msg_count     how many messages the turn holds
    turns.token_count   their token estimate (SUM of messages.tokens)
    messages.tokens     per-message token estimate, computed once at append time

    The runtime no longer keeps a conversation in memory, so the counts it used to read
    off a live queue are now columns. v1 rows have no such columns; without them the v2
    INSERTs fail, which is why the store refuses a v1 file outright.

SAFETY
    * Refuses to run while an edgeclaw process holds the workspace lock (two writers would
      overwrite each other's session state -- the store is strict about this too).
    * Copies the database, its -wal and -shm to a timestamped backup BEFORE opening it for
      writing, and verifies the backup is readable.
    * Everything happens in ONE transaction: a failure leaves the file exactly as it was.
    * Idempotent: a v2 file is reported and left alone.
    * --dry-run reports what it would do without writing.

TOKEN ESTIMATE
    Must reproduce src/kernel/context/edc_token_estimate.c exactly, or the backfilled
    number would disagree with the ones the runtime writes later:
        msg = 8 (frame overhead) + ascii_chars // 4 + wide_chars
    walking every VALUE of the message, skipping the `ts` / `render` / `id` keys (metadata
    that is stripped before the request is sent, so counting it would add phantom tokens).

USAGE
    python3 tools/migrate_edgeclaw_db_v1_to_v2.py <workspace>            # migrate
    python3 tools/migrate_edgeclaw_db_v1_to_v2.py <workspace> --dry-run  # report only
    python3 tools/migrate_edgeclaw_db_v1_to_v2.py <workspace> --check    # verify only

    <workspace> is the directory holding edgeclaw.db (e.g. ~/.edgeclaw/workspace).
"""

import argparse
import fcntl
import json
import os
import shutil
import sqlite3
import sys
import time

DB_NAME = "edgeclaw.db"
V1 = 1
V2 = 2

# Mirrors EDC_TOKEN_MSG_OVERHEAD in edc_token_estimate.c.
MSG_OVERHEAD = 8
SKIP_KEYS = ("ts", "render", "id")


# ── token estimate (must match edc_token_estimate.c) ──────────────────────────


def _count_chars(s):
    """(ascii_chars, wide_chars) for one string, the way est_count() counts them."""
    ascii_n = 0
    wide_n = 0
    for ch in s:
        if ord(ch) < 0x80:
            ascii_n += 1
        else:
            # One non-ASCII character counts once, however many UTF-8 bytes it uses.
            wide_n += 1
    return ascii_n, wide_n


def _walk(node, acc):
    """est_walk_values(): every VALUE, recursing through arrays and objects."""
    if isinstance(node, dict):
        for key, val in node.items():
            if key in SKIP_KEYS:
                continue
            _walk_value(val, acc)
    elif isinstance(node, list):
        for val in node:
            _walk_value(val, acc)


def _walk_value(val, acc):
    if isinstance(val, str):
        a, w = _count_chars(val)
        acc[0] += a
        acc[1] += w
    elif isinstance(val, (dict, list)):
        _walk(val, acc)
    # numbers / bools / null contribute nothing, same as cJSON in the C version


def estimate_msg(msg):
    if not isinstance(msg, dict):
        return 0
    acc = [0, 0]
    _walk(msg, acc)
    return MSG_OVERHEAD + acc[0] // 4 + acc[1]


# ── helpers ───────────────────────────────────────────────────────────────────


def die(msg, code=1):
    print("error: %s" % msg, file=sys.stderr)
    sys.exit(code)


def check_not_running(db_path):
    """The store takes flock(LOCK_EX) on the database path. If we can take it, nobody is
    running; if we cannot, a live agent owns the file and migrating would race it."""
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
        dst = "%s.v1.%s%s" % (db_path, stamp, suffix)
        shutil.copy2(src, dst)
        made.append(dst)
    # The backup must be readable before we are allowed to touch the original.
    con = sqlite3.connect("file:%s?mode=ro" % made[0], uri=True)
    try:
        n = con.execute("select count(*) from messages").fetchone()[0]
        ok = con.execute("pragma quick_check(1)").fetchone()[0]
    finally:
        con.close()
    if ok != "ok":
        die("backup %s failed quick_check: %s" % (made[0], ok))
    print("backup      : %s (%d message rows, quick_check ok)" % (made[0], n))
    return made


def columns(con, table):
    return {row[1] for row in con.execute("pragma table_info(%s)" % table)}


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


# ── the migration ─────────────────────────────────────────────────────────────


def migrate(db_path, dry_run):
    if not os.path.exists(db_path):
        die("%s does not exist -- nothing to migrate" % db_path)

    if not check_not_running(db_path):
        die(
            "another edgeclaw instance is using this workspace (the database is locked).\n"
            "       Stop it first: the migration must be the only writer."
        )

    # Read-only look first, so a wrong database is rejected before any backup noise.
    ro = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        ver = ro.execute("pragma user_version").fetchone()[0]
        print("database    : %s" % db_path)
        print("found       : schema v%s" % ver)
        if ver == V2:
            print("\nalready v2 -- nothing to do.")
            return 0
        if ver != V1:
            die("expected schema v1 or v2, found v%s; refusing to guess" % ver)
    finally:
        ro.close()

    backup_paths = backup(db_path) if not dry_run else []

    con = sqlite3.connect(db_path, isolation_level=None)
    con.execute("pragma foreign_keys=OFF")  # ALTER TABLE must not trip the FK cascade
    con.execute("begin immediate")
    try:
        # ── 1. additive DDL ──
        tcols = columns(con, "turns")
        mcols = columns(con, "messages")
        added = []
        if "msg_count" not in tcols:
            con.execute("alter table turns add column msg_count integer not null default 0")
            added.append("turns.msg_count")
        if "token_count" not in tcols:
            con.execute("alter table turns add column token_count integer not null default 0")
            added.append("turns.token_count")
        if "tokens" not in mcols:
            con.execute("alter table messages add column tokens integer not null default 0")
            added.append("messages.tokens")
        print("added       : %s" % (", ".join(added) if added else "(none, columns already present)"))

        # ── 2. per-message token estimates ──
        rows = con.execute(
            "select id, json from messages where tokens = 0"
        ).fetchall()
        print("tokenizing  : %d message rows" % len(rows))
        bad = 0
        updates = []
        for msg_id, raw in rows:
            try:
                msg = json.loads(raw)
            except (ValueError, TypeError):
                bad += 1
                continue
            updates.append((estimate_msg(msg), msg_id))
        con.executemany("update messages set tokens = ? where id = ?", updates)
        if bad:
            print("              (%d unparseable rows left at 0 tokens)" % bad)

        # ── 3. per-turn aggregates, from the rows themselves ──
        con.execute(
            "update turns set "
            "  msg_count = (select count(*) from messages m "
            "               where m.task_id = turns.task_id and m.turn_ord = turns.turn_ord), "
            "  token_count = (select coalesce(sum(m.tokens), 0) from messages m "
            "                 where m.task_id = turns.task_id and m.turn_ord = turns.turn_ord)"
        )
        print("aggregated  : turns.msg_count / turns.token_count")

        # ── 4. stamp the version ──
        con.execute("pragma user_version = %d" % V2)

        if dry_run:
            raise _Rollback()

        con.execute("commit")
        print("committed   : schema v2")
    except _Rollback:
        con.execute("rollback")
        print("\n--dry-run: rolled back, database untouched")
        return 0
    except Exception:
        con.execute("rollback")
        raise
    finally:
        con.close()

    # Fold the WAL back so the migration leaves one file behind.
    con = sqlite3.connect(db_path)
    try:
        con.execute("pragma wal_checkpoint(truncate)")
    finally:
        con.close()

    if not verify(db_path, quiet=False):
        print(
            "\nbackup kept at:\n  %s\nrestore with: cp <backup> %s" % ("\n  ".join(backup_paths), db_path)
        )
        return 1
    return 0


class _Rollback(Exception):
    """Raised inside the transaction to undo a --dry-run."""


def verify(db_path, quiet=True):
    """Read the file the way the v2 binary will, and cross-check the aggregates."""
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    ok = True
    try:
        ver = con.execute("pragma user_version").fetchone()[0]
        if ver != V2:
            print("verify      : FAILED user_version=%s, expected %d" % (ver, V2))
            return False

        # Columns the v2 writer/reader depend on must exist.
        for table, col in (("turns", "msg_count"), ("turns", "token_count"), ("messages", "tokens")):
            if col not in columns(con, table):
                print("verify      : FAILED missing %s.%s" % (table, col))
                ok = False

        # Every turn's aggregate must match its rows.
        mismatched = con.execute(
            "select count(*) from turns t "
            "where t.msg_count <> (select count(*) from messages m "
            "                      where m.task_id=t.task_id and m.turn_ord=t.turn_ord) "
            "   or t.token_count <> (select coalesce(sum(m.tokens),0) from messages m "
            "                        where m.task_id=t.task_id and m.turn_ord=t.turn_ord)"
        ).fetchone()[0]
        if mismatched:
            print("verify      : FAILED %d turns disagree with their message rows" % mismatched)
            ok = False

        # An empty estimate would mean the tokenizer silently did nothing.
        zero = con.execute("select count(*) from messages where tokens = 0").fetchone()[0]
        total = con.execute("select count(*) from messages").fetchone()[0]

        if not quiet:
            msgs = con.execute("select coalesce(sum(tokens),0) from messages").fetchone()[0]
            turns = con.execute("select count(*) from turns").fetchone()[0]
            sessions = con.execute("select count(*) from sessions").fetchone()[0]
            print("verify      : schema v%d, %d sessions, %d turns, %d messages, %d tokens"
                  % (ver, sessions, turns, total, msgs))
            if zero:
                print("              (%d messages carry 0 tokens: empty content is normal)" % zero)
    except sqlite3.Error as e:
        print("verify      : FAILED %s" % e)
        ok = False
    finally:
        con.close()
    return ok


def main():
    ap = argparse.ArgumentParser(description="Migrate an edgeclaw workspace database v1 -> v2")
    ap.add_argument("workspace", help="directory containing edgeclaw.db")
    ap.add_argument("--dry-run", action="store_true", help="report and roll back")
    ap.add_argument("--check", action="store_true", help="verify only, never write")
    args = ap.parse_args()

    db_path = os.path.join(os.path.expanduser(args.workspace), DB_NAME)
    if not os.path.exists(db_path):
        die("%s does not exist (%s)" % (DB_NAME, db_path))

    if args.check:
        con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
        try:
            print("database    : %s" % db_path)
            report(con)
        finally:
            con.close()
        return 0 if verify(db_path, quiet=False) else 1

    try:
        return migrate(db_path, args.dry_run)
    except sqlite3.Error as e:
        die("sqlite: %s" % e)


if __name__ == "__main__":
    sys.exit(main())
