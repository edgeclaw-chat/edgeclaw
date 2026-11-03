# Limitations

What EdgeClaw does not do, and what it does not do *well yet*. This is deliberately blunt:
a limitation nobody wrote down becomes a bug report, and a limitation written down vaguely
("experimental") is not a limitation at all.

If something here is wrong — a feature listed as missing that now exists — that is a bug in
this file. Fix the file in the same commit that adds the feature.

Last reviewed: 2026-11-02 (v2.0.0 release preparation).

---

## Not implemented, and not planned for 1.x

These are decisions, not backlog items. The reasoning lives in
[`product-alignment.zh.md`](product-alignment.zh.md).

| Capability | Why not |
|---|---|
| **MCP servers** | A "minimal, extensible harness" rival ships MCP as an extension, not as core; wiring it in would also break the "single binary, no runtime" story, because `npx`/`uvx`/`docker` servers need a runtime the user must supply. Reopening conditions are recorded in `product-alignment.zh.md` §3.1. If a user's reason for not trying EdgeClaw is MCP, that is the signal to reopen it — say so in an issue. |
| **Sub-agents** | Not "missing": EdgeClaw's task graph (fork, dependencies, joins) covers the same ground with a different model. A separate sub-agent concept would be a second answer to a question that already has one. |
| **Plan mode** | Same: a task tree with explicit states is already the plan, and it is inspectable, unlike a mode. |
| **Project trust (`/trust`)** | Two-level project resources are **on by default** here. That is a deliberate trade (prompt-injection surface for zero-config convenience), and the "code execution" hole it would gate — packages, MCP server commands — does not exist yet. Revisit if packages land. |
| **TypeScript / embedded-JS extension runtime** | Incompatible with a self-contained binary. See `product-alignment.zh.md`. |
| **`--mode rpc` / an SDK** | `edgeclaw_cli` covers scripted use by accepting "the same line you would type in the TUI". A long-running RPC surface is a different product with different stability requirements. |
| **Web search / fetch tools** | EdgeClaw has none, and neither does the project it measures itself against. Use `exec_shell` with `curl`. |

## Implemented partially — know the edges

### Remote devices

`device_shell` / `device_upload` / `device_download` talk to a separate agent over TCP or a
PPPoS serial link. Two real constraints:

- **The device tools do not exist on Windows.** The `device` module is not linked into the
  Windows binary at all (`src/tui/BUILD` selects it away), so the tools are absent rather
  than broken, and the model never sees them. PPPoS is POSIX-only on top of that.
- The agent spawns with `execl("/bin/sh", "sh", "-c", cmd)` — it inherits **its whole
  environment**. The `exec_shell` *tool* goes through an allowlist
  (`src/sandbox/edc_shell_env.c`); the device agent does not. Do not run an agent on a host
  whose environment carries credentials you would not hand to the remote side.

### The sandbox

- `--no-sandbox` / `sandbox.enable=false` exists, and `/sandbox list` shows what is enforced.
- Enforcement differs per platform: `kernel_sandbox` is true only on Linux and macOS
  (`src/common/edc_platform.c`: "no backend for this system yet" for everything else). Where
  it is false the switch is clamped off, `/sandbox on` is refused and the CLI override does
  not take effect — so on Windows you get the path checks and nothing more.
- The sandbox constrains *tool* file and shell access. It does not constrain what the model
  can be *told*: `@file` reads go through the same checks, but a prompt can still describe
  anything the model already saw.

### Folding (long conversations)

Compaction produces a summary plus a `<read-files>` / `<modified-files>` list. That list is
**a snapshot taken at the moment the fold happened**, not a live view:

- later edits are not reflected;
- `exec_shell` never records anything, so `sed -i`, `cat` and `git checkout` leave no trace —
  in one real session a group had 917 `exec_shell` calls against 1 `read_file`, and the list
  was empty not because nothing was read but because everything was read through the shell.

Treat `/group files` as a hint, not as `git status`. The output always carries this
disclaimer for exactly that reason.

### Session memory

Memory entries are curated per session and injected into requests. Two things to know:

- The catalogue ("what exists") and the change feed are separate; a task that has never seen
  the catalogue gets it once, and that flag is now explicit state (`memo_baseline_done`).
- Entries deleted in a session are deleted in the store too — there is no undo.

### `exec_shell` on the model path

- Output is capped (2000 lines / `tool_max_output_size`, default 10000 bytes). Past the cap
  the **whole** output is written to a spill file and the message reports where it went, so a
  truncated result is a move, not a loss.
- A non-zero exit is reported at the top of the result (`[exit N]` / `[signal N]`). It does
  **not** fail the tool call or the task: the task still shows COMPLETED, and reading the
  status is the reader's job.
- Commands get no tty. Interactive programs (`sudo`, `vim`, `ssh` with a password prompt)
  fail immediately rather than hanging.

### Persisted state

- One process per workspace. A second instance is refused, not queued.
- The workspace must be on a **local** filesystem. NFS/SMB are refused because SQLite
  locking is unreliable there; so is anything whose `statfs` type is not on the allowlist
  (this includes FUSE and virtiofs — a case where that showed
  up as a confusing test failure).
- An **older** schema is migrated in place on startup, with a backup written first. A
  **newer** schema is refused, and the message says to update EdgeClaw — never to delete
  anything. Downgrading the binary is not supported and cannot be.

## Not verified

Stated plainly, because "it compiles" is not evidence:

| Area | State |
|---|---|
| Line coverage | ~80%, function ~91%, **branch ~59%** (measured at `dev@92761dc` + `test_setup_dialog`, 128 tests; see `coverage-handover.zh.md`). The remaining gap is concentrated in paths that are hard to force deterministically: allocation failures, write errors at an exact moment, disk-full, sandbox denials. The LLM 4xx/5xx and HTTPS transport failures are now covered (§5.9, §5.10), and so is the first-run wizard (§5.11). |
| Windows | Builds in CI. The PPPoS transport and the kernel sandbox do not exist there. Not exercised by hand on real hardware for this release. |
| armv7 | Built locally (cross) and **run under `qemu-arm`**: the released shape of the binary starts and reports `platform linux/arm`. Not run on real armv7 hardware, and the Android device agent is compiled only. |
| Real-model behaviour | The suite drives a fake LLM. Prompt and tool-choice quality is not asserted anywhere. |
| macOS | Builds in CI (aarch64). Seatbelt sandbox paths are compiled, not exercised. |

## Known sharp edges

Not "unverified" — verified, and wrong in a narrow case:

- **A local incremental build can report a stale `--version`.** The version string is
  generated by a rule that runs `git describe`, and Bazel cannot see git, so the rule's
  inputs are only `src/common/BUILD`'s content. Once generated, the header is reused until
  that file changes, and a binary rebuilt afterwards keeps naming the older commit — or
  `-dirty` from a tree that has since been cleaned. Released archives are unaffected: they are
  built from a fresh checkout, and every release verifies `--version` on the downloaded
  artefact. Locally, `touch src/common/BUILD` (or `bazel clean`) refreshes it.
  `stamp = 1` alone does not force the re-run, and Bazel 8.0.0 does not expand
  workspace-status variables in `genrule` commands, so the proper fix — a volatile input — is
  not available yet. This is why it is written down instead of fixed.

## Fixed at v2.0.0 that used to be limitations

Listed because older docs and issues still describe these as open:

- **A schema bump no longer costs you your history.** Migrations run in the binary, with a
  verified backup.
- **`--version` exists**, and a schema refusal names the version that wrote the database.
- **Tests run in CI**, including `manual`-tagged targets (which had rotted unnoticed) and the
  whole suite under AddressSanitizer/UBSan.
- **The release archive carries its licences** (`THIRD_PARTY_NOTICES.md` + `licenses/`).
- **A failing shell command says so** (`[exit N]`).

### Also fixed in v2.0.1

- **`--version` no longer names a commit that was not built**, for released archives. The
  rule ran inside Bazel's execroot — symlinks everywhere, including `.git` — so `git status`
  saw every file as typechanged and `git describe --dirty` called a clean tree dirty.
- **A markdown hard break is a line break.** Two trailing spaces used to render as a space,
  collapsing poems, addresses and hand-aligned text into one run.
- **The trunk is built by CI.** A push to `dev` started nothing before v2.0.1.

