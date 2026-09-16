#!/usr/bin/env python3
"""One-shot migration: session.json schema v3 -> v4.

Why this exists
---------------
v3-era files can carry two kinds of pre-transcript leftovers, and the *runtime*
used to clean both of them up on the fly:

  * the legacy `system` group/task (the container that once held messages
    belonging to no task: command echo, command output, scheduler notices).
    Nothing creates it any more since the TUI owns its own transcript, so the
    loader simply dropped it (`EDC_LEGACY_SYSTEM_ID` in edc_state_serializer.c).
  * the 5c display metadata `displayed` / `header` on tool messages.  Messages
    are round-tripped as raw JSON (not rebuilt field by field), so unknown keys
    never fell away by themselves; `turn_to_json` deleted them from the copy it
    wrote.

Both were permanent compatibility branches in the load/save path, kept alive
only for data that can no longer be produced.  This script performs the cleanup
once and bumps the schema to 4, so the loader can simply REFUSE anything older
and old files fail loudly (and are quarantined) instead of being silently
patched up on every load.  This mirrors the v1 -> v2 migration (see
docs/session-model-v3.md §10).

What it does, per session.json
------------------------------
  1. drop the legacy `system` group and `system` task, plus any group left
     without members once those are gone;
  2. drop `displayed` / `header` from every message object (any object carrying
     a `role` key, wherever it sits);
  3. set `version` to 4.

Safety
------
A file that would change is first copied to `session.json.v3.bak` (refusing to
overwrite an existing backup), then rewritten via a temp file + rename.  The
rewrite keeps the on-disk format the app itself writes: compact JSON, UTF-8
(no \\uXXXX escapes), no trailing newline.  After writing, the file is re-read
and verified (version == 4, no `system` ids left, no display keys left); on
mismatch the backup is restored.  Idempotent: a v4 file is skipped untouched.
A version other than 3 or 4 is refused, never guessed at.

Usage
-----
Nothing is written unless --yes is given:

  python3 tools/migrate_session_v3_to_v4.py ~/.edgeclaw/workspace        # dry run
  python3 tools/migrate_session_v3_to_v4.py --yes ~/.edgeclaw/workspace
  python3 tools/migrate_session_v3_to_v4.py --yes ~/.edgeclaw/workspace/s7/session.json

Each PATH may be a session.json, a session directory, or a workspace directory
(every */session.json below it, skipping *.bak / *.broken).
"""

import argparse
import glob
import json
import os
import shutil
import sys

SCHEMA_FROM = 3
SCHEMA_TO = 4
SYSTEM_IDS = {"system"}
BACKUP_SUFFIX = ".v3.bak"
DISPLAY_KEYS = ("displayed", "header")


def resolve_targets(paths):
    out = []
    for p in paths:
        p = os.path.expanduser(p)
        if os.path.isdir(p):
            if os.path.basename(p) and os.path.exists(os.path.join(p, "session.json")):
                out.append(os.path.join(p, "session.json"))
            else:
                out.extend(sorted(glob.glob(os.path.join(p, "*/session.json"))))
        elif os.path.isfile(p):
            out.append(p)
        else:
            sys.stderr.write("skip (not found): %s\n" % p)
    return [p for p in out if not p.endswith(".bak") and not p.endswith(".broken")]


def strip_system(obj):
    """Drop the legacy `system` group/task. Returns (groups, tasks) removed."""
    groups = obj.get("groups", [])
    tasks = obj.get("tasks", [])
    members = {}
    for t in tasks:
        members.setdefault(t.get("group_id"), []).append(t)

    def is_stale_task(t):
        return t.get("task_id") in SYSTEM_IDS or t.get("group_id") in SYSTEM_IDS

    def is_stale_group(g):
        return g.get("group_id") in SYSTEM_IDS or any(is_stale_task(t) for t in members.get(g.get("group_id"), []))

    stale_groups = [g for g in groups if is_stale_group(g)]
    stale_group_ids = {g.get("group_id") for g in stale_groups}
    stale_tasks = [t for t in tasks if is_stale_task(t) or t.get("group_id") in stale_group_ids]
    removed_ids = {t.get("task_id") for t in stale_tasks}

    # A non-system group whose members we just removed would linger as an empty row.
    for g in groups:
        if g in stale_groups:
            continue
        left = [t for t in members.get(g.get("group_id"), []) if t.get("task_id") not in removed_ids]
        if not left and members.get(g.get("group_id")):
            stale_groups.append(g)
            stale_group_ids.add(g.get("group_id"))

    if not stale_groups and not stale_tasks:
        return 0, 0

    obj["tasks"] = [t for t in tasks if t.get("task_id") not in removed_ids]
    obj["groups"] = [g for g in groups if g.get("group_id") not in stale_group_ids]

    # Scrub references the removed ids may have left behind.
    for g in obj["groups"]:
        members_left = g.get("member_task_ids")
        if isinstance(members_left, list):
            g["member_task_ids"] = [i for i in members_left if i not in removed_ids and i not in SYSTEM_IDS]
    sess = obj.get("session")
    if isinstance(sess, dict):
        if sess.get("active_task_id") in removed_ids or sess.get("active_task_id") in SYSTEM_IDS:
            sess.pop("active_task_id", None)
        if sess.get("foreground_group_id") in stale_group_ids or sess.get("foreground_group_id") in SYSTEM_IDS:
            sess.pop("foreground_group_id", None)
    return len(stale_groups), len(stale_tasks)


def strip_display_keys(node):
    """Delete `displayed`/`header` from every message object (one with `role`)."""
    removed = 0
    if isinstance(node, dict):
        if "role" in node:
            for k in DISPLAY_KEYS:
                if k in node:
                    del node[k]
                    removed += 1
        for v in node.values():
            removed += strip_display_keys(v)
    elif isinstance(node, list):
        for v in node:
            removed += strip_display_keys(v)
    return removed


def count_system_ids(obj):
    n = 0
    for t in obj.get("tasks", []):
        if t.get("task_id") in SYSTEM_IDS or t.get("group_id") in SYSTEM_IDS:
            n += 1
    for g in obj.get("groups", []):
        if g.get("group_id") in SYSTEM_IDS:
            n += 1
    return n


def count_display_keys(obj):
    n = 0
    for task in obj.get("tasks", []):
        turns = list(task.get("turns") or []) + ([task["active_turn"]] if task.get("active_turn") else [])
        for turn in turns:
            for msg in turn.get("messages") or []:
                if isinstance(msg, dict):
                    n += sum(1 for k in DISPLAY_KEYS if k in msg)
    return n


def main():
    ap = argparse.ArgumentParser(description="Migrate session.json from schema v3 to v4 (one-shot legacy cleanup).")
    ap.add_argument("paths", nargs="+", help="session.json, session dir, or workspace dir")
    ap.add_argument("--yes", action="store_true", help="actually write (backup first); default is a dry run")
    args = ap.parse_args()

    targets = resolve_targets(args.paths)
    if not targets:
        sys.stderr.write("no session.json found\n")
        return 1

    changed = 0
    for path in targets:
        with open(path, encoding="utf-8") as fh:
            obj = json.load(fh)

        ver = obj.get("version")
        if ver == SCHEMA_TO:
            print("current  %s  (already v%d)" % (path, SCHEMA_TO))
            continue
        if ver != SCHEMA_FROM:
            sys.stderr.write("refusing %s: version is %r, expected %d\n" % (path, ver, SCHEMA_FROM))
            return 1

        groups, tasks = strip_system(obj)
        keys = strip_display_keys(obj)
        obj["version"] = SCHEMA_TO

        detail = "group(s)=%d task(s)=%d display-key(s)=%d" % (groups, tasks, keys)
        if not args.yes:
            print("would migrate %s  v%d->v%d  %s" % (path, SCHEMA_FROM, SCHEMA_TO, detail))
            changed += 1
            continue

        backup = path + BACKUP_SUFFIX
        if os.path.exists(backup):
            sys.stderr.write("refusing to overwrite existing backup: %s\n" % backup)
            return 1
        shutil.copy2(path, backup)

        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(obj, fh, ensure_ascii=False, separators=(",", ":"))
        os.replace(tmp, path)

        # Verify the written file: right version, and every legacy artefact gone.
        with open(path, encoding="utf-8") as fh:
            after = json.load(fh)
        if after.get("version") != SCHEMA_TO or count_system_ids(after) or count_display_keys(after):
            shutil.copy2(backup, path)
            sys.stderr.write("verification failed (restored from backup): %s\n" % path)
            return 1

        print("migrated %s  v%d->v%d  %s  (backup: %s)" % (path, SCHEMA_FROM, SCHEMA_TO, detail,
                                                           os.path.basename(backup)))
        changed += 1

    print("%s %d file(s)" % ("migrate" if args.yes else "would migrate", changed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
