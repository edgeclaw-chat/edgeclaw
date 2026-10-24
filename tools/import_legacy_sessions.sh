#!/usr/bin/env bash
# Import the legacy <workspace>/<session_id>/session.json files into edgeclaw.db, then
# (only when asked) delete them.
#
# WHY A SCRIPT: the importer has to run with NO edgeclaw process holding the workspace,
# and the order matters -- back up, import, verify, and only then delete the source files.
# Doing that by hand once is fine; doing it while a running agent also owns the database is
# how a conversation gets lost.
#
# USAGE
#   tools/import_legacy_sessions.sh <workspace> [--delete-json]
#
#   <workspace>     e.g. ~/.edgeclaw/workspace  (holds edgeclaw.db and the s*/ dirs)
#   --delete-json   after a successful import, move the session.json files to a timestamped
#                   backup directory and remove the session directories that the importer
#                   has taken over. Without this flag the source files are left alone.
#
# The importer binary is built by bazel:
#   bazel build //tests:import_session_json

set -euo pipefail

WS="${1:-}"
DELETE_JSON=0
[ "${2:-}" = "--delete-json" ] && DELETE_JSON=1

if [ -z "$WS" ]; then
    echo "usage: $0 <workspace> [--delete-json]" >&2
    exit 2
fi
WS="${WS/#\~/$HOME}"
[ -d "$WS" ] || { echo "not a directory: $WS" >&2; exit 2; }
[ -f "$WS/edgeclaw.db" ] || { echo "no edgeclaw.db in $WS" >&2; exit 2; }

HERE="$(cd "$(dirname "$0")" && pwd)"
IMPORTER="$HERE/../bazel-bin/tests/import_session_json"
[ -x "$IMPORTER" ] || { echo "build it first: bazel build //tests:import_session_json" >&2; exit 2; }

# ── 1. refuse to run against a live workspace ────────────────────────────────
# The store takes flock(LOCK_EX) on the database; so does this. A quick non-invasive check
# first, so the user gets a clear message instead of a failed write.
if command -v fuser >/dev/null 2>&1; then
    if fuser "$WS/edgeclaw.db" >/dev/null 2>&1; then
        echo "another edgeclaw process is using $WS/edgeclaw.db -- stop it first." >&2
        exit 1
    fi
fi
if pgrep -f "edgeclaw( |\$)" >/dev/null 2>&1; then
    echo "warning: an edgeclaw process appears to be running; the import will refuse if it"
    echo "         holds the lock. Stop it first if this is the workspace it is using." >&2
fi

# ── 2. back up the database ─────────────────────────────────────────────────
STAMP="$(date +%Y%m%d-%H%M%S)"
for suffix in "" "-wal" "-shm"; do
    [ -f "$WS/edgeclaw.db$suffix" ] && cp -p "$WS/edgeclaw.db$suffix" "$WS/edgeclaw.db.pre-import.$STAMP$suffix"
done
echo "backup      : edgeclaw.db.pre-import.$STAMP"
python3 - "$WS/edgeclaw.db.pre-import.$STAMP" <<'PY'
import sqlite3, sys
con = sqlite3.connect("file:%s?mode=ro" % sys.argv[1], uri=True)
ok = con.execute("pragma quick_check(1)").fetchone()[0]
print("              quick_check:", ok)
sys.exit(0 if ok == "ok" else 1)
PY

# ── 3. import every session directory that still has a session.json ─────────
# Sessions registered in sessions.json are the ones the UI shows; an unregistered one is
# still imported (its data is real and this is the only chance to keep it), it just will
# not appear in the session list until it is added there.
#
# A session that ALREADY has rows in the database is skipped. The importer's flush is
# "upsert this snapshot and delete what is not in it", so feeding it an old file over a
# newer live session would delete every task the running conversation added since — which
# is exactly the case for the session you are talking to right now.
in_db() {
    python3 - "$WS/edgeclaw.db" "$1" <<'PY'
import sqlite3, sys
con = sqlite3.connect("file:%s?mode=ro" % sys.argv[1], uri=True)
n = con.execute("select count(*) from sessions where session_id=?", (sys.argv[2],)).fetchone()[0]
sys.exit(0 if n > 0 else 1)
PY
}

failed=0
skipped=""
imported=""
for dir in "$WS"/s*/; do
    sid="$(basename "$dir")"
    [ -f "$dir/session.json" ] || continue
    if in_db "$sid"; then
        echo
        echo "=== $sid === SKIPPED: already has rows in the database"
        echo "    (importing this file would delete whatever the live session added since)"
        skipped="$skipped $sid"
        continue
    fi
    echo
    echo "=== $sid ==="
    if "$IMPORTER" "$WS" "$sid"; then
        imported="$imported $sid"
    else
        echo "IMPORT FAILED for $sid -- left untouched" >&2
        failed=$((failed + 1))
    fi
done

if [ -n "$skipped" ]; then
    echo
    echo "skipped (already in db):$skipped"
fi

if [ -z "$imported" ]; then
    echo
    echo "nothing imported (no session.json left)."
    exit 0
fi
if [ "$failed" -ne 0 ]; then
    echo
    echo "$failed session(s) failed to import; NOT deleting anything." >&2
    exit 1
fi

echo
echo "imported    :$imported"

# ── 4. delete the sources, only when asked and only after a clean import ────
if [ "$DELETE_JSON" -eq 0 ]; then
    echo
    echo "sources kept. Re-run with --delete-json once you have checked the sessions open."
    exit 0
fi

TRASH="$WS/.legacy-session-json.$STAMP"
mkdir -p "$TRASH"
for sid in $imported; do
    # ONLY session.json*: it is the file this tool took over, and the only one the current
    # build refuses to read. The sibling chat.jsonl is still live -- the TUI loads and
    # appends it to draw a conversation (edc_tui_window.cpp), independently of the store --
    # so removing it would blank the transcript panel of a session whose history imported
    # perfectly.
    for f in "$WS/$sid"/session.json*; do
        [ -e "$f" ] || continue
        mv "$f" "$TRASH/${sid}__$(basename "$f")"
    done
    echo "removed     : $sid/session.json* (chat.jsonl kept -- the TUI still reads it)"
done
echo
echo "old session.json files moved to $TRASH"
echo "delete that directory once you are sure:  rm -rf '$TRASH'"
