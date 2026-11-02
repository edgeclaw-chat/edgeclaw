#!/usr/bin/env python3
"""Rename session directories from "s<id>" to "<id>".

WHY
    A session id is an INTEGER in the database now. For a while the text form of it was
    "s<id>" -- a leftover from when session ids were the strings "s7", "s13". That meant an
    id had TWO text forms ("15" and "s15"), and picking the wrong renderer compiled cleanly
    and silently wrote a session's transcript into a directory that did not exist.

    The code now has one form: the integer. This renames the directories to match, so the
    directory name, what the UI shows, and what the database stores are all the same string.

WHAT IT DOES, IN ORDER
    1. merges a stray "<id>" directory INTO an existing "s<id>" one, when both exist. That
       combination is the signature of the old bug: output went to <id>/ while user messages
       went to s<id>/. (The merge itself lives in merge_stray_session_dir.py; this script
       calls the same logic.)
    2. renames "s<id>" to "<id>" when "<id>" does not already exist.
    3. leaves anything it cannot decide alone and says so.

MUST RUN WITH NO EDGECLAW PROCESS
    A running process holds the workspace lock, keeps writing to the old paths, and would
    race the rename. The script refuses unless --force is given (for a copy).

USAGE
    python3 tools/rename_session_dirs.py <workspace> [--dry-run] [--force]
"""

import argparse
import os
import shutil
import sys
import time


def die(msg, code=1):
    print("error: %s" % msg, file=sys.stderr)
    sys.exit(code)


def workspace_free(db_path):
    import fcntl
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


def chat_lines(path):
    if not os.path.exists(path):
        return 0, 0
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
        body = f.read()
    n = body.count("\n") + (0 if body.endswith("\n") else 1) if body else 0
    return n, len(body.encode("utf-8", errors="surrogateescape"))


def merge_into(src_dir, dst_dir, dry_run):
    """Append src_dir/chat.jsonl onto dst_dir/chat.jsonl, then drop src_dir."""
    src = os.path.join(src_dir, "chat.jsonl")
    dst = os.path.join(dst_dir, "chat.jsonl")
    lines, size = chat_lines(src)
    print("    merge %s -> %s (%d lines, %d bytes)"
          % (os.path.basename(src_dir), os.path.basename(dst_dir), lines, size))
    if dry_run:
        return

    with open(src, "r", encoding="utf-8", errors="surrogateescape") as f:
        body = f.read()

    if os.path.exists(dst):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        shutil.copy2(dst, "%s.before-merge.%s" % (dst, stamp))

    need_newline = False
    if os.path.exists(dst) and os.path.getsize(dst) > 0:
        with open(dst, "rb") as f:
            f.seek(-1, os.SEEK_END)
            need_newline = f.read(1) != b"\n"

    with open(dst, "a", encoding="utf-8", errors="surrogateescape") as out:
        if need_newline:
            out.write("\n")
        out.write(body if (not body or body.endswith("\n")) else body + "\n")

    shutil.rmtree(src_dir)


def main():
    ap = argparse.ArgumentParser(description='Rename session directories "s<id>" to "<id>"')
    ap.add_argument("workspace")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="proceed even though an edgeclaw process may hold the workspace")
    args = ap.parse_args()

    ws = os.path.expanduser(args.workspace)
    if not os.path.isdir(ws):
        die("not a directory: %s" % ws)
    db = os.path.join(ws, "edgeclaw.db")

    print("workspace   : %s" % ws)
    if not workspace_free(db):
        if not args.force:
            die("an edgeclaw process holds this workspace.\n"
                "       Stop it first: it keeps writing to the old paths.\n"
                "       (--force exists for a copy, not for the live workspace.)")
        print("warning     : the database is locked; proceeding anyway (--force)")

    dirs = [d for d in os.listdir(ws) if os.path.isdir(os.path.join(ws, d))]
    legacy = sorted((d for d in dirs if d.startswith("s") and d[1:].isdigit()),
                    key=lambda d: int(d[1:]))
    plain = {d for d in dirs if d.isdigit()}

    if not legacy:
        already = sorted(plain, key=lambda d: int(d))
        print("\nno s<id> directories left. Plain numeric ones: %s"
              % (", ".join(already) if already else "(none)"))
        return 0

    print("\nfound %d legacy director%s: %s"
          % (len(legacy), "y" if len(legacy) == 1 else "ies", ", ".join(legacy)))

    merged = renamed = skipped = 0
    for legacy_name in legacy:
        num = legacy_name[1:]
        src = os.path.join(ws, legacy_name)
        dst = os.path.join(ws, num)
        print("\n  %s -> %s" % (legacy_name, num))

        if os.path.isdir(dst):
            # Both exist: the old bug wrote output into <num>/ while user messages went to
            # s<num>/. Fold the stray one in, then the name is free.
            print("    %s/ already exists: merging it first" % num)
            merge_into(dst, src, args.dry_run)
            merged += 1
            if args.dry_run:
                continue
        if args.dry_run:
            print("    would rename")
            renamed += 1
            continue
        if os.path.exists(dst):
            print("    SKIPPED: %s still exists after the merge" % num)
            skipped += 1
            continue
        os.rename(src, dst)
        print("    renamed")
        renamed += 1

    if args.dry_run:
        print("\n--dry-run: %d merge(s), %d rename(s) would happen, nothing written"
              % (merged, renamed))
    else:
        print("\nmerged %d, renamed %d, skipped %d." % (merged, renamed, skipped))
        print("Restart edgeclaw; the transcript paths in the UI follow the new names.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
