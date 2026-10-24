#!/usr/bin/env python3
"""Merge a stray bare-number session directory into its "s<id>" sibling.

WHAT THIS REPAIRS
    Between v6 (session ids became numbers) and the fix that followed it, the channel actor
    rendered session ids with edc_id_str() -- "15" instead of "s15". The TUI derives a
    session's transcript path from that string, so assistant / thought / tool output landed
    in <workspace>/15/chat.jsonl while user messages went to <workspace>/s15/chat.jsonl
    through the displayed session. The chat pane then showed user bubbles only.

    The code is fixed; this repairs the files it left behind.

WHAT IT DOES
    Appends <ws>/<n>/chat.jsonl to <ws>/s<n>/chat.jsonl, line by line, then removes the
    stray directory. A backup of the target is written first.

ORDERING
    Transcript entries carry no timestamp, so the two files can only be concatenated. The
    stray file holds only the output produced while the bug was live (a short window), so
    appending it at the end puts it at the end of the conversation -- close enough to read,
    and nothing is lost.

MUST RUN WITH NO EDGECLAW PROCESS
    The live process keeps appending to the stray file (it is the process that has the bug).
    Merging while it runs races its writes and leaves a gap; the script refuses unless
    --force is given.

USAGE
    python3 tools/merge_stray_session_dir.py <workspace> [--dry-run] [--force]

    <workspace>  holds the session directories and edgeclaw.db
"""

import argparse
import os
import shutil
import sys
import time


def die(msg, code=1):
    print("error: %s" % msg, file=sys.stderr)
    sys.exit(code)


def probe_lock(db_path):
    """True when nothing holds the database."""
    import fcntl
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


def find_strays(ws):
    out = []
    for name in sorted(os.listdir(ws)):
        path = os.path.join(ws, name)
        if not os.path.isdir(path) or not name.isdigit():
            continue
        target = os.path.join(ws, "s%s" % name)
        if not os.path.isdir(target):
            print("  skip %s: no %s sibling to merge into" % (name, "s%s" % name))
            continue
        src = os.path.join(path, "chat.jsonl")
        if not os.path.exists(src):
            print("  skip %s: no chat.jsonl" % name)
            continue
        out.append((name, path, target, src, os.path.join(target, "chat.jsonl")))
    return out


def merge_one(stray_name, stray_dir, target_dir, src, dst, dry_run):
    lines = 0
    with open(src, "r", encoding="utf-8", errors="surrogateescape") as f:
        body = f.read()
    if body:
        lines = body.count("\n") + (0 if body.endswith("\n") else 1)
    size = len(body.encode("utf-8", errors="surrogateescape"))

    print("  %s/chat.jsonl (%d lines, %d bytes) -> %s/chat.jsonl"
          % (stray_name, lines, size, os.path.basename(target_dir)))
    if dry_run:
        return lines

    if os.path.exists(dst):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        backup = "%s.before-merge.%s" % (dst, stamp)
        shutil.copy2(dst, backup)
        print("    backup      : %s" % os.path.basename(backup))

    # The target must not end mid-line, or the first merged entry would be glued onto the
    # last one (the appender already defends against this on the write side).
    need_newline = False
    if os.path.exists(dst) and os.path.getsize(dst) > 0:
        with open(dst, "rb") as f:
            f.seek(-1, os.SEEK_END)
            need_newline = f.read(1) != b"\n"

    with open(dst, "a", encoding="utf-8", errors="surrogateescape") as out:
        if need_newline:
            out.write("\n")
        out.write(body if body.endswith("\n") or not body else body + "\n")
    print("    appended    : %d lines" % lines)

    shutil.rmtree(stray_dir)
    print("    removed     : %s/" % stray_name)
    return lines


def main():
    ap = argparse.ArgumentParser(
        description="Merge a stray bare-number session directory into its s<id> sibling")
    ap.add_argument("workspace")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="merge even though an edgeclaw process may be running (races its writes)")
    args = ap.parse_args()

    ws = os.path.expanduser(args.workspace)
    db = os.path.join(ws, "edgeclaw.db")
    if not os.path.isdir(ws):
        die("not a directory: %s" % ws)

    print("workspace   : %s" % ws)
    if os.path.exists(db) and not probe_lock(db):
        if not args.force:
            die("an edgeclaw process holds this workspace.\n"
                "       It is the process with the bug and keeps appending to the stray file.\n"
                "       Stop it first, or pass --force to accept a gap.")
        print("warning     : the database is locked; merging anyway (--force)")

    strays = find_strays(ws)
    if not strays:
        print("\nnothing to merge.")
        return 0

    print("\nstray session directories with an s<id> sibling:")
    total = 0
    for stray_name, stray_dir, target_dir, src, dst in strays:
        total += merge_one(stray_name, stray_dir, target_dir, src, dst, args.dry_run)

    if args.dry_run:
        print("\n--dry-run: %d line(s) would be merged, nothing written" % total)
    else:
        print("\nmerged %d line(s). Restart edgeclaw and the transcript is whole." % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
