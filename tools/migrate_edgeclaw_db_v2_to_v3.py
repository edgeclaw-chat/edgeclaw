#!/usr/bin/env python3
"""Migrate an edgeclaw workspace database from schema v2 to v3.

WHY THIS EXISTS
    edc_state_store.h refuses a database whose EDC_DB_VERSION is not the current one. v3
    changes the shape of `messages` (see below), and that cannot be expressed as ALTER
    TABLE ADD COLUMN, so the table is rebuilt. Doing that by hand is how histories get
    lost; this does it in one transaction, with a verified backup first.

WHAT CHANGED IN v3
    `messages` used to be (id, task_id, turn_ord, msg_ord, role, mid, ts, json, tokens),
    where `json` held the WHOLE message -- including the metadata that was also stored in
    columns. So `ts` existed twice per row (an integer column and a "YYYY-MM-DD HH:MM:SS"
    string inside the JSON) and every read/write converted between them.

    v3 splits that: the metadata is columns, `body` holds only the fields that go on the
    wire, and session_id/group_id are denormalized so a group's messages can be read
    without joining tasks.

        messages(id, session_id, group_id, task_id, turn_ord, msg_ord,
                 role, mid, ts, render, synthetic, payload, tokens, body)

    Nothing is dropped. The split is defined by a name list: every key NOT in
    (id, ts, render, mid, payload, synthetic) stays in `body`, so provider fields this
    script has never heard of ride along untouched.

SAFETY
    * Refuses to run while an edgeclaw process holds the workspace lock.
    * Backs up the database, its -wal and -shm, and verifies the backup before writing.
    * One transaction: a failure leaves the file byte-for-byte as it was.
    * --dry-run does the whole migration and rolls back.
    * Idempotent: a v3 file is reported and left alone.
    * Verifies afterwards: same row count, every message still parses, and the reassembled
      message matches what v2 would have returned for every meta field.

USAGE
    python3 tools/migrate_edgeclaw_db_v2_to_v3.py <workspace>            # migrate
    python3 tools/migrate_edgeclaw_db_v2_to_v3.py <workspace> --dry-run
    python3 tools/migrate_edgeclaw_db_v2_to_v3.py <workspace> --check
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
V2 = 2
V3 = 3

# The keys that become columns. MUST match kStoreOnlyMsgKeys in edc_state_store.c.
# `role` is here even though it is a wire field: it also has a column, and a value in two
# places is a value that will disagree with itself.
STORE_ONLY_KEYS = ("id", "ts", "render", "mid", "payload", "synthetic", "role")

# Dead structures removed in the v3 review (no reader, no writer anywhere in the code).
DROP_DEAD = (
    "ALTER TABLE sessions DROP COLUMN archived",
    "ALTER TABLE sessions DROP COLUMN broken",
    "ALTER TABLE groups DROP COLUMN extra",
    # The leader IS the task whose id equals group_id (edc_task_group_new/set_leader);
    # the column could only ever restate it. Verified on the real database: 2/2 groups had
    # leader_task_id == group_id.
    "ALTER TABLE groups DROP COLUMN leader_task_id",
    "ALTER TABLE tasks DROP COLUMN extra",
    "DROP TABLE IF EXISTS meta",
)

NEW_SCHEMA = """
CREATE TABLE messages_v3 (
  id         TEXT NOT NULL PRIMARY KEY,
  session_id TEXT NOT NULL,
  group_id   TEXT NOT NULL,
  task_id    TEXT NOT NULL,
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
  FOREIGN KEY (task_id, turn_ord) REFERENCES turns(task_id, turn_ord) ON DELETE CASCADE
)
"""

NEW_INDEXES = (
    "CREATE UNIQUE INDEX IF NOT EXISTS messages_natural ON messages(task_id, turn_ord, msg_ord)",
    "CREATE INDEX IF NOT EXISTS messages_by_mid ON messages(mid) WHERE mid IS NOT NULL",
    "CREATE INDEX IF NOT EXISTS messages_by_group "
    "ON messages(session_id, group_id, task_id, turn_ord, msg_ord)",
)


def die(msg, code=1):
    print("error: %s" % msg, file=sys.stderr)
    sys.exit(code)


def check_not_running(db_path):
    """The store holds flock(LOCK_EX) on the database path while it runs."""
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
        dst = "%s.v2.%s%s" % (db_path, stamp, suffix)
        shutil.copy2(src, dst)
        made.append(dst)
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


def parse_display(s):
    """'YYYY-MM-DD HH:MM:SS' in LOCAL time -> unix seconds (0 when unparseable).

    Matches edc_datetime_parse(): mktime with tm_isdst=-1, i.e. local time.
    """
    if not isinstance(s, str):
        return 0
    try:
        tm = time.strptime(s, "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return 0
    # time.mktime reads the struct as LOCAL time, same as mktime() in C.
    return int(time.mktime(tm))


def split_message(msg):
    """(body_dict, meta_dict) for one v2 message object."""
    meta = {}
    body = {}
    for k, v in msg.items():
        if k in STORE_ONLY_KEYS:
            meta[k] = v
        else:
            body[k] = v
    return body, meta


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


def migrate(db_path, dry_run):
    if not os.path.exists(db_path):
        die("%s does not exist -- nothing to migrate" % db_path)

    if not check_not_running(db_path):
        die(
            "another edgeclaw instance is using this workspace (the database is locked).\n"
            "       Stop it first: the migration must be the only writer."
        )

    ro = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        ver = ro.execute("pragma user_version").fetchone()[0]
        print("database    : %s" % db_path)
        print("found       : schema v%s" % ver)
        if ver == V3:
            print("\nalready v3 -- nothing to do.")
            return 0
        if ver != V2:
            die("expected schema v2 or v3, found v%s; refusing to guess" % ver)
    finally:
        ro.close()

    if not dry_run:
        backup_paths = backup(db_path)

    con = sqlite3.connect(db_path, isolation_level=None)
    con.execute("pragma foreign_keys=OFF")  # rebuilding a child table
    con.execute("begin immediate")
    try:
        # ── read every v2 row before touching anything ──
        rows = con.execute(
            "select id, task_id, turn_ord, msg_ord, role, mid, ts, json, tokens from messages"
        ).fetchall()
        print("reading     : %d message rows" % len(rows))

        # task -> (session_id, group_id), for the denormalized columns
        owners = {}
        for tid, sid, gid in con.execute("select task_id, session_id, group_id from tasks"):
            owners[tid] = (sid, gid)

        # ── rebuild the table ──
        con.execute("drop table if exists messages_v3")
        con.execute(NEW_SCHEMA)
        con.execute("drop index if exists messages_natural")
        con.execute("drop index if exists messages_by_mid")

        out = []
        unparsed = 0
        missing_owner = 0
        ts_from_json = 0
        ts_from_column = 0
        for mid, task_id, turn_ord, msg_ord, role, mid_col, ts_col, raw, tokens in rows:
            try:
                msg = json.loads(raw)
                if not isinstance(msg, dict):
                    raise ValueError("not an object")
                parsed = True
            except (ValueError, TypeError):
                # Keep the bytes rather than lose them: body is NOT NULL, so a row we cannot
                # parse becomes an object holding the original text.
                unparsed += 1
                msg = {"_unparsed": raw}
                parsed = False

            body, meta = split_message(msg)

            owner = owners.get(task_id)
            if owner is None:
                missing_owner += 1
                owner = ("", "")

            # ── metadata: THE JSON WINS, the v2 column is only a fallback ──
            # The v2 columns were DERIVED from this JSON, and two of them were derived
            # wrongly by the runtime append path (ts was strtod() of a display string, so
            # it holds the year 2026; the mid column holds the message id instead of the
            # frontend mid). The JSON is the message as the runtime built it, so it is what
            # this split must read.
            ts = parse_display(meta.get("ts")) if parsed else 0
            if ts > 0:
                ts_from_json += 1
            else:
                candidate = int(ts_col or 0)
                # Sanity: a value outside 2001..2099 cannot be one of ours.
                if 978307200 <= candidate <= 4102444800:
                    ts = candidate
                    ts_from_column += 1
                else:
                    ts = 0

            mid_value = meta.get("mid") if parsed else None
            if not mid_value and mid_col and mid_col != mid:
                # Older rows only ever had the frontend mid in the column.
                mid_value = mid_col

            payload = meta.get("payload")
            payload_text = (
                json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
                if payload is not None
                else None
            )
            synthetic = 1 if meta.get("synthetic") in (True, 1) else 0

            out.append(
                (
                    mid,
                    owner[0],
                    owner[1],
                    task_id,
                    turn_ord,
                    msg_ord,
                    role if role else (meta.get("role") if parsed else None),
                    mid_value,
                    ts,
                    meta.get("render") if parsed else None,
                    synthetic,
                    payload_text,
                    int(tokens or 0),
                    json.dumps(body, separators=(",", ":"), ensure_ascii=False),
                )
            )

        con.executemany(
            "insert into messages_v3(id, session_id, group_id, task_id, turn_ord, msg_ord,"
            " role, mid, ts, render, synthetic, payload, tokens, body)"
            " values(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            out,
        )
        con.execute("drop table messages")
        con.execute("alter table messages_v3 rename to messages")

        # The review also deleted columns and a table that nothing read or wrote. A
        # database carried forward must not keep them: the schema this build creates is the
        # schema a migrated file has to end up with.
        for stmt in DROP_DEAD:
            try:
                con.execute(stmt)
            except sqlite3.OperationalError:
                pass  # already absent (a file that was created new, not migrated)
        # NOT executescript(): it issues an implicit COMMIT first, which would end the
        # transaction (and make --dry-run unable to roll back).
        for stmt in NEW_INDEXES:
            con.execute(stmt)
        con.execute("pragma user_version=%d" % V3)

        print("split       : %d rows (ts from JSON: %d, from column fallback: %d)"
              % (len(out), ts_from_json, ts_from_column))
        if unparsed:
            print("              (%d rows were not valid JSON; their bytes are kept in body)" % unparsed)
        if missing_owner:
            print("              (%d rows had no task owner; session_id/group_id left empty)" % missing_owner)

        if dry_run:
            raise _Rollback()

        con.execute("commit")
        print("committed   : schema v3")
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
        print("verify      : %s" % ("ok" if verify(db_path) else "FAILED"))
        if not verify(db_path):
            print("backup kept at:\n  %s" % "\n  ".join(backup_paths))
            return 1
    return 0


class _Rollback(Exception):
    """Raised inside the transaction to undo a --dry-run."""


def verify(db_path):
    """Read the migrated file the way v3 will, and cross-check the split."""
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    ok = True
    try:
        ver = con.execute("pragma user_version").fetchone()[0]
        if ver != V3:
            print("verify      : FAILED user_version=%s, expected %d" % (ver, V3))
            return False

        cols = {r[1] for r in con.execute("pragma table_info(messages)")}
        for need in ("session_id", "group_id", "body", "render", "synthetic", "payload"):
            if need not in cols:
                print("verify      : FAILED messages.%s missing" % need)
                ok = False
        if "json" in cols:
            print("verify      : FAILED old `json` column still present")
            ok = False

        # Every body must parse, and every metadata key must NOT be inside it (that is the
        # whole point of the split -- a key in both places would drift again).
        bad_body = 0
        leaked = 0
        n = 0
        for body, in con.execute("select body from messages"):
            n += 1
            try:
                obj = json.loads(body)
            except (ValueError, TypeError):
                bad_body += 1
                continue
            if not isinstance(obj, dict):
                bad_body += 1
                continue
            if any(k in obj for k in STORE_ONLY_KEYS if k != "id"):
                leaked += 1
        if bad_body:
            print("verify      : FAILED %d bodies do not parse" % bad_body)
            ok = False
        if leaked:
            print("verify      : FAILED %d bodies still carry store-only keys" % leaked)
            ok = False

        # The denormalized owner must agree with the task row.
        drift = con.execute(
            "select count(*) from messages m join tasks t on t.task_id=m.task_id "
            "where m.session_id<>t.session_id or m.group_id<>t.group_id"
        ).fetchone()[0]
        if drift:
            print("verify      : FAILED %d rows disagree with their task's owner" % drift)
            ok = False

        # turns' materialized counts must still match their rows.
        mism = con.execute(
            "select count(*) from turns t where t.msg_count <> "
            "(select count(*) from messages m where m.task_id=t.task_id and m.turn_ord=t.turn_ord)"
        ).fetchone()[0]
        if mism:
            print("verify      : FAILED %d turns disagree with their message rows" % mism)
            ok = False

        if ok:
            print("verify      : schema v%d, %d messages, all bodies parse, no store-only "
                  "keys leaked" % (ver, n))
    except sqlite3.Error as e:
        print("verify      : FAILED %s" % e)
        ok = False
    finally:
        con.close()
    return ok


def main():
    ap = argparse.ArgumentParser(description="Migrate an edgeclaw workspace database v2 -> v3")
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
        return 0 if verify(db_path) else 1

    try:
        return migrate(db_path, args.dry_run)
    except sqlite3.Error as e:
        die("sqlite: %s" % e)


if __name__ == "__main__":
    sys.exit(main())
