#!/usr/bin/env python3
"""Strip the legacy "system" group/task from persisted session.json files.

Background.  The v3 model used to carry a session-level container — the `system`
group with its `system` task (`is_system = true`, always IDLE) — for messages that
belong to no task: command echo, command output, scheduler SYS notices.  It
existed so the TUI could rebuild its chat log by deriving it from session.json.
The TUI now records its own transcript (`<session>/chat.jsonl`, see
docs/session-model-v3.md §12) and nothing derives the chat log any more, so the
container lost its only writer and its structure was removed from the model:
`edc_task.is_system`, `edc_session_get_or_create_system_task`,
`edc_session_add_system_message`, the serialized `is_system` key and the
user-facing enumeration skips are all gone.

That leaves old files holding an object nothing can create, hide or clean up:
the `system` task is IDLE, so the serializer's transient-state filter now drops
it — but its group would survive as an empty, visible group in the sidebar.

This script removes those groups (and any member task marked `is_system` or named
`system`) from the files it is pointed at.

Safety.  A file that would change is first copied to
`session.json.pre-system-cleanup.bak` (refusing to overwrite an existing
backup), then rewritten in place via a temp file + rename.  The rewrite keeps the
on-disk format the agent itself writes: compact JSON, UTF-8 (no \\uXXXX escapes),
no trailing newline.  Idempotent: a file with nothing to strip is left untouched.

Usage.  Nothing is written unless --yes is given:

  python3 tools/strip_system_group.py --yes ~/.edgeclaw/workspace
  python3 tools/strip_system_group.py --yes ~/.edgeclaw/workspace/s7/session.json
  python3 tools/strip_system_group.py ~/.edgeclaw/workspace      # dry run

Each PATH may be a session.json, a session directory, or a workspace directory
(every */session.json below it, skipping *.bak / *.broken).
"""

import argparse
import glob
import json
import os
import shutil
import sys

SYSTEM_IDS = {"system"}
BACKUP_SUFFIX = ".pre-system-cleanup.bak"
# 5c-era display metadata; harmless (unknown keys are ignored on load and simply
# not written back on the next save), so it is only reported, never rewritten.
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


def count_message_keys(obj):
    """Report the leftover 5c display metadata (tool messages)."""
    found = 0
    for task in obj.get("tasks", []):
        turns = list(task.get("turns") or []) + ([task["active_turn"]] if task.get("active_turn") else [])
        for turn in turns:
            for msg in turn.get("messages") or []:
                if any(k in msg for k in DISPLAY_KEYS):
                    found += 1
    return found


def strip(obj):
    """Return (removed_groups, removed_tasks, dropped_messages). Mutates |obj|."""
    groups = obj.get("groups", [])
    tasks = obj.get("tasks", [])
    members = {}
    for t in tasks:
        members.setdefault(t.get("group_id"), []).append(t)

    def is_stale_task(t):
        return bool(t.get("is_system")) or t.get("task_id") in SYSTEM_IDS

    def is_stale_group(g):
        return g.get("group_id") in SYSTEM_IDS or any(is_stale_task(t) for t in members.get(g.get("group_id"), []))

    stale_groups = [g for g in groups if is_stale_group(g)]
    stale_group_ids = {g.get("group_id") for g in stale_groups}
    stale_tasks = [t for t in tasks if is_stale_task(t) or t.get("group_id") in stale_group_ids]
    removed_ids = {t.get("task_id") for t in stale_tasks}

    # A group whose members we just removed would linger as an empty row.
    for g in groups:
        if g in stale_groups or g.get("group_id") in stale_group_ids:
            continue
        left = [t for t in members.get(g.get("group_id"), []) if t.get("task_id") not in removed_ids]
        if not left and members.get(g.get("group_id")):
            stale_groups.append(g)
            stale_group_ids.add(g.get("group_id"))

    if not stale_groups and not stale_tasks:
        return 0, 0, 0

    dropped = 0
    for task in stale_tasks:
        turns = list(task.get("turns") or []) + ([task["active_turn"]] if task.get("active_turn") else [])
        for turn in turns:
            dropped += len(turn.get("messages") or [])

    obj["tasks"] = [t for t in tasks if t.get("task_id") not in removed_ids]
    obj["groups"] = [g for g in groups if g.get("group_id") not in stale_group_ids]
    return len(stale_groups), len(stale_tasks), dropped


def main():
    ap = argparse.ArgumentParser(description="Strip the legacy system group/task from session.json files.")
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

        groups, tasks, dropped = strip(obj)
        display_keys = count_message_keys(obj)
        note = "  (%d message(s) still carry %s — harmless, they go away on the next save)" % (
            display_keys, "/".join(DISPLAY_KEYS)) if display_keys else ""

        if not groups and not tasks:
            print("clean    %s%s" % (path, note))
            continue

        detail = "group(s)=%d task(s)=%d message(s)=%d" % (groups, tasks, dropped)
        if not args.yes:
            print("would fix %s  %s%s" % (path, detail, note))
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

        # Prove the rewrite only dropped what we meant to drop: re-applying the
        # same removal to the backup must reproduce the written file exactly.
        with open(backup, encoding="utf-8") as fh:
            expect = json.load(fh)
        with open(path, encoding="utf-8") as fh:
            after = json.load(fh)
        strip(expect)  # what the file must look like after the removal
        if expect != after:
            shutil.copy2(backup, path)
            sys.stderr.write("verification failed (restored from backup): %s\n" % path)
            return 1
        print("fixed    %s  %s  (backup: %s)%s" % (path, detail, os.path.basename(backup), note))
        changed += 1

    print("%s %d file(s)" % ("fix" if args.yes else "would fix", changed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
