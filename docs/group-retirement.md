# Group retirement (archive / restore / rm) — design

**Status: proposal. No code changed.** Reviewed against `dev` at `8c14713`
(2026-11-23). This is the SQLite state-store build (`edgeclaw.db`,
`EDC_DB_VERSION 16`), NOT the old `session.json` fork.

Goal: let groups retire out of the sidebar as they accumulate, without losing their
knowledge, and make retirement reversible until an explicit confirmed hard delete.

## 0. Why the previous design was wrong

An earlier draft assumed whole-tree `session.json` serialization. That is the old
`edgeclaw-test` fork. The current build persists through
`src/kernel/session/edc_state_store.c`: `flush` upserts every in-memory group/task and
then `delete_stale()` deletes DB rows not marked seen in `flush_seen_group` /
`flush_seen_task`. Everything below is grounded in that mechanism.

## 1. Model and the single most important invariant

Add to `struct edc_task_group` (`src/kernel/task/edc_task_group.h`):

```c
bool is_archived;
uint64_t archived_at;  /* epoch seconds, 0 while active; display only */
```

with one setter `edc_task_group_set_archived(group, bool)` (idempotent; stamps the
clock on the false→true edge only) and a query `edc_task_group_is_archived()`.

**Invariant: an archived group stays in the in-memory tree (`session->groups`) with
all its tasks, turns and dependency edges.** Only the display and placement layers
treat it differently. This is what makes retirement safe under the DB store:

- `flush` walks `session->groups`, runs `INSERT_GROUP` and `mark_seen_group()` for each
  (`edc_state_store.c` ~1915–1945), so an archived group keeps its DB row and is never
  swept by `delete_stale()`. No flush special-casing is needed.
- All its tasks stay resident and keep their edges, so a cross-group `|`/`-d` pipe
  into or out of an archived group still works during retirement — exactly as before.
- `rm` (hard delete) is the *only* operation that removes a group from the tree; that
  is when `delete_stale()` legitimately cleans its row and cascades its turns/messages.

`is_archived` is deliberately NOT `is_orphaned` (kernel-set when the leader leaves)
and NOT the system `kind`. A group may be archived and orphaned at once; a system
group is never archivable.

State machine:

```
active ──archive──▶ archived ──rm --yes──▶ gone (row cascaded away)
   ▲                   │
   └────restore────────┘
```

## 2. Persistence: DB migration v16 → v17

The `groups` table currently has no archived column. (The `archived`/`broken` words in
a comment inside `edc_state_store.c` refer to *sessions*-table placeholders that were
later replaced by `memo_next_seq` et al — there is no live column.) One migration:

- `EDC_DB_VERSION` 16 → 17 (`edc_state_store.h`).
- `step_v16_to_v17` (`edc_state_migrate.c`): `ALTER TABLE groups ADD COLUMN is_archived
  INTEGER NOT NULL DEFAULT 0` plus `ALTER TABLE groups ADD COLUMN archived_at INTEGER
  NOT NULL DEFAULT 0`. O(1), consistent with v10–v14 ADD COLUMN steps.
- `verify_v17`: assert both columns exist (same style as `verify_v16`).
- Register `{16, step_v16_to_v17, verify_v17}` in `EDC_MIGRATIONS[]`.
- `EDC_SCHEMA_SQL` in `edc_state_store.c`: add both columns to the fresh-DB `groups`
  DDL — a migration and a fresh workspace must agree (`test_state_migrate.c` asserts
  this).
- `INSERT_GROUP` / `bind_group` (~1230): add `is_archived`, `archived_at` to the column
  list, VALUES, `ON CONFLICT ... DO UPDATE SET`, and the bind indices.
- load query (~2513) and its column reads (~2560): select and set both fields with
  0/false defaults for old rows (the ADD COLUMN default already covers migrated rows).

`memory` stays DB-only and is untouched by this feature. `archived_at` survives
restart because it is a persisted column. `idle_since` is ALSO persisted (see §6):
flush writes it, load restores it after members are attached, so a restart does not
reset the `--idle` window.

## 3. Commands

Same dispatch table as today: `k_group_subs[]` in `src/shell/edc_commands.c` (now
`edc_sub_argv_entry` with pre-parsed argv). Group ids are `int64_t`; resolve like
`/group fold` does: `edc_session_store_resolve_task(sm, sched, strtoll(id), &owner)`
then `edc_session_find_group(owner, t->group_id)`.

| Command | Behavior |
|---|---|
| `/group archive <id> [--force] [--no-harvest] [--curate]` | retire a group; there is no `retire` alias — one verb only |

`--curate` is opt-in (confirmed): the default harvest is the deterministic seed only;
an LLM curation round is never automatic, including in batch mode.
| `/group restore <id>` | un-hide; take foreground if none |
| `/group list [--all]` | active only; `--all` adds `[archived] <when>` |
| `/group rm <id> --yes` | hard delete; **only archived**, `--yes` mandatory |
| `/group archive --idle <30d\|12h> [--dry-run] [--force] [--no-harvest] [--curate]` | batch |

### archive, in order (order is the safety property)

1. Resolve; refuse system group, already-archived group, unknown id (never fall back to foreground).
2. If any member is not `edc_task_is_finished()` and no `--force`: refuse, listing them.
   With `--force`, `TASK_SIGTERM` each via `edc_scheduler_send_signal_to_task_ptr()` — the
   `/task stop` path, so JOIN/SEQUENCE cascade and in-flight tool calls get interrupted
   results.
3. `--curate` needs the one-system-task-per-session slot
   (`edc_scheduler_has_pending_system_task`); decide **before** mutating anything.
4. Harvest: the group memory body is DB-only — read it with
   `edc_state_store_group_memory_load(session_id, group_id, ...)`. If non-empty, seed one
   deterministic session-memory entry via `edc_session_memory_create_entry()` titled
   `Archived group <id>`, proposed_by `archive`. `--no-harvest` skips. `--curate`
   additionally calls `edc_session_memory_start(sched, session, group, NULL)` to merge
   into existing entries; the seed is what guarantees no loss if curate fails.
5. `edc_task_group_set_archived(group, true)`.
6. If it was the foreground group, call `edc_scheduler_set_foreground()` on the best
   non-system / non-archived / non-orphaned / non-empty group (prefer highest leader
   `completion_seq`), and clear `session->active_task` if it pointed into the old group.
7. Persist: the archive flag is in-memory until the next `flush`. For a command issued
   at a message boundary the normal scheduler flush covers it; if commands can return
   before the next flush in some path, call the store flush explicitly from the command
   handler (verify against `edc_commands` → scheduler flush wiring before coding).

### Batch `--idle` — reuse `idle_since`, not message timestamps

The clock is `group->idle_since`: "the wall-clock moment busy_members last dropped to
zero", maintained on every terminal transition / attach / scan by
`edc_task_group_refresh_busy()`. Its header comment explicitly rejects the
newest-message timestamp as a false "long silent" signal while a user is typing.
`--idle <dur>` selects groups with `idle_since != 0 && now - idle_since >= dur`.

- `idle_since` IS persisted (v17, §2): flush writes it, load restores it after members
  are attached. So a restart no longer resets the window — a group idle for 30 days
  stays eligible across restarts. (L2 extraction also reads this clock; the only side
  effect is that a long-idle group may satisfy the extract window right after a restart,
  queued one system task at a time.)
- Skip foreground, system, already-archived groups; skip groups with alive members
  unless `--force`; `--dry-run` prints matches and changes nothing.
- Duration parser accepts `<n>d`/`<n>h` only; malformed → usage error.

### `restore`

Clear the flag via the same setter; if the session has no foreground group, take it.
The group never left the tree, so nothing is re-attached; the next flush persists the
flag change.

### `rm` (hard delete)

Only archived + `--yes`. Drop cross-group dependency edges in both directions with
`edc_task_drop_dep()` (it marks edges cancelled without unlinking, so a
`LL_FOREACH_SAFE` traversal is safe), clear foreground/active pointers, then
`edc_session_remove_group()` + `edc_task_group_free()`. The next `flush` runs
`delete_stale()`, which removes the group row; the `ON DELETE CASCADE` foreign keys
clean turns/messages/fold-files. The harvested memo entry survives. Note in the reply
that a JOIN dependent loses its material injection once the edge is dropped.

## 4. Rejection changes

Add `|| group->is_archived` / `|| edc_task_group_is_archived(g)` to the three placement
predicates: `edc_scheduler.c:629`, `edc_interp.c:156`, `edc_shell.c:662`. For
`-g/--group <id>` resolution (`edc_commands.c` ~3048 in the prompt/skill path, plus the
`/task new` path), refuse an archived group with a "restore it first" message instead
of placing work there. `fold` on an archived group is refused (no reason to fold a
retired group's memory). `info`/`history` stay readable on archived groups.

## 5. TUI

- Snapshot (`edc_scheduler.c` ~1972): add `"archived"` bool next to `"orphaned"`.
- DTO (`edc_task_snapshot.hpp` `TaskGroup`) + `ParseGroup` in `edc_task_snapshot.cpp`:
  add `bool archived = false`.
- `TaskTreeModel::Apply()` (`edc_task_tree_model.cpp:69`): `if (g.archived) continue;`
  at the top of the loop, so archived groups vanish from the sidebar and their task
  transitions cannot raise unread markers. No sidebar toggle / footer cell (rare
  action; discovery is `/group list --all`).

## 6. Help / docs / tests

- `k_cmd_docs` (~399) and the bare-`/group` usage/unknown-verb text: add all new verbs.
- `docs/edgeclaw.md`, `docs/edgeclaw.1`, `README.md`: command tables + a short
  "Retiring groups" section (soft archive, knowledge seed, batch, confirmed rm).
- Tests:
  - `tests/test_state_migrate.c`: v16→v17 step + fresh-schema agreement + verify.
  - `tests/test_state_store_differential.c` (or `test_state_store_open.c`): group row
    round-trips `is_archived`/`archived_at`; a group missing from the tree is still
    deleted by `delete_stale` while an archived one present in the tree is kept.
  - new command test modeled on `test_commands_group_fold.c`: archive/restore/rm,
    guards, `--force` cancel, `--dry-run`, memo seed, foreground move, edge dropping.
  - `test_task_tree_model` / `test_task_snapshot`: archived group hidden / parsed.

## 7. Risks and explicit non-goals

- **Store thread rule**: all new store calls run on the single runner thread, same as
  existing command paths (`edc_state_store.h` invariant; see also m193/m129).
- **Bigger blast radius than the JSON fork**: a schema bump touches migration, fresh
  DDL, INSERT/bind, load, and their tests, all in one commit. That is the established
  pattern (v14/v16 did exactly this).
- **Unbound workdir / quarantine**: unrelated; no interaction with v16's workdir change.
- Non-goals: no auto-archiving timer (batch is an explicit command), no LLM harvest by
  default (deterministic seed, `--curate` opt-in), no sidebar "show archived" toggle in
  this iteration.
