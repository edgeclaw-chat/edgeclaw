# Limitations

What EdgeClaw does not do, and what it does not do *well yet*. This is deliberately blunt:
a limitation nobody wrote down becomes a bug report, and a limitation written down vaguely
("experimental") is not a limitation at all.

If something here is wrong — a feature listed as missing that now exists — that is a bug in
this file. Fix the file in the same commit that adds the feature.

Last reviewed: 2026-11-06 (v1.0.0 release preparation).

---

## Not implemented, and not planned for 1.x

These are decisions, not backlog items.

| Capability | Why not |
|---|---|
| **MCP servers** | Wiring MCP in would break the "single binary, no runtime" story, because `npx`/`uvx`/`docker` servers need a runtime the user must supply. If a user's reason for not trying EdgeClaw is MCP, that is the signal to reopen it — say so in an issue. |
| **Sub-agents** | Not "missing": EdgeClaw's task graph (fork, dependencies, joins) covers the same ground with a different model. A separate sub-agent concept would be a second answer to a question that already has one. |
| **Plan mode** | Same: a task tree with explicit states is already the plan, and it is inspectable, unlike a mode. |
| **Project trust (`/trust`)** | Two-level project resources are **on by default** here. That is a deliberate trade (prompt-injection surface for zero-config convenience), and the "code execution" hole it would gate — packages, MCP server commands — does not exist yet. Revisit if packages land. |
| **TypeScript / embedded-JS extension runtime** | Incompatible with a self-contained binary. |
| **`--mode rpc` / an SDK** | There is no scripted frontend: `edgeclaw` is the only binary, and it is interactive. A long-running RPC surface is a different product with different stability requirements, so it would be designed rather than bolted on. |
| **Web search / fetch tools** | EdgeClaw has none. Use `exec_shell` with `curl`. |

## Not shipped yet

### Remote devices

The `device_shell` / `device_upload` / `device_download` tools are **not registered**, so
the model never sees them and no release ships a device agent. The remote-agent half of the
feature is still being built; a tool the model can reach is a promise, and there is nothing
for it to talk to yet.

What that means in practice: `device_*` appearing in a `config.json` (`device_transport`,
`device_ip`, …) sets nothing, because no tool reads it. The code is compiled and kept
(`src/toolbox/edc_toolbox.c` is two commented lines away from turning it back on), and the
`device/` module is still linked, so this is a closed door rather than a removal.

Two constraints worth recording for whoever finishes it:

- **Windows gets no device tools by design.** The `device` module is not linked into the
  Windows binary (`src/tui/BUILD` selects it away), and PPPoS is POSIX-only on top of that.
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

#### It is a blast-radius limit, not a confidentiality boundary

Worth stating plainly, because the two get conflated and the difference decides what you
may safely put on the machine.

- **What it does buy.** A tool or a shell command cannot reach a path the lists do not name,
  cannot write outside the writable ones, and cannot read a denied one. That bounds what one
  bad command, or one injected instruction, can touch.
- **What it does not buy.** Anything the model *does* read goes into its context and is sent
  to the API provider. The deny list stops a file from being read at all; it cannot stop
  what was already read from travelling. Treat "the model can reach it" and "it has left
  the machine" as the same statement.
- **Root removes most of it.** Deny masks are applied inside a mount namespace created by a
  helper that inherits the caller's privileges. Run as root and the whitelisted paths are
  readable regardless of their permission bits — `~/.ssh`'s 0600, a `root:shadow` hash file.
  A warning is logged at startup when this is the case; run as an unprivileged user if the
  distinction matters.
- **Deny is matched on paths.** A second name for the same inode is handled for the tool
  read path, but the boundary is the path prefix, not the file's identity. Treat every list
  as a guard rail against mistakes and accidents; if something must not be readable at all,
  do not keep it where a process running as you can read it.

Correspondingly, the built-in defaults deny the app's own key material
(`config.json`, `.key_hash`), the conventional credential dotfiles, shell history,
and — even though `/etc` stays readable so tools keep working — the system files whose
permission bits already implied they were off limits (`/etc/shadow`, `/etc/sudoers`,
`/etc/ssl/private`, the ssh host keys).

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
| Line coverage | **63%** — what [Codecov](https://app.codecov.io/github/edgeclaw-chat/edgeclaw) reports for `master`, over 25933 lines across 133 files. It counts a line whose branches were not all taken as **not** covered, which is why it reads lower than the ~81% an lcov `LH/LF` total gives for the same run (80.9%); the difference is the partially-executed lines. The exact counts move by a line or two between runs, because a few paths are timing-dependent — the percentage does not. For reference, the lcov report also measures functions 92.1% and branches 59.8%; Codecov reports neither. The remaining gap is concentrated in paths that are hard to force deterministically: allocation failures, write errors at an exact moment, disk-full, sandbox denials. The LLM 4xx/5xx and HTTPS transport failures are covered, so are the first-run wizard, the frontend's C surface, `exec_shell`'s timeout/abort paths and the store's id-collision / measured-flush paths. |
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

## Fixed before v1.0.0, and no longer limitations

Listed because older docs and issues still describe these as open:

- **A schema bump no longer costs you your history.** Migrations run in the binary, with a
  verified backup.
- **`--version` exists**, and a schema refusal names the version that wrote the database.
- **Tests run in CI**, including `manual`-tagged targets (which had rotted unnoticed) and the
  whole suite under AddressSanitizer/UBSan.
- **The release archive carries its licences** (`THIRD_PARTY_NOTICES.md` + `licenses/`).
- **A failing shell command says so** (`[exit N]`).

### Also fixed before this release, later in the run-up

- **`--version` no longer names a commit that was not built**, for released archives. The
  rule ran inside Bazel's execroot — symlinks everywhere, including `.git` — so `git status`
  saw every file as typechanged and `git describe --dirty` called a clean tree dirty.
- **A markdown hard break is a line break.** Two trailing spaces used to render as a space,
  collapsing poems, addresses and hand-aligned text into one run.
- **The trunk is built by CI.** A push to `dev` used to start no workflow at all; only
  pull requests and release tags did.

