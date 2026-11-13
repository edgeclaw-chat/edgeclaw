# Limitations

What EdgeClaw does not do, and what it does not do *well yet*. This is deliberately blunt:
a limitation nobody wrote down becomes a bug report, and a limitation written down vaguely
("experimental") is not a limitation at all.

If something here is wrong — a feature listed as missing that now exists — that is a bug in
this file. Fix the file in the same commit that adds the feature.

Last reviewed: 2026-11-12 (v1.0.0 release preparation).

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

## Updating

The check, the banner and `/update` ship in this release, so what they do *not* cover belongs
here rather than in an issue tracker.

**Only the installer's own copies can update themselves.** `edc_upgrade_detect()` recognises
exactly `$HOME/.local/bin` and `$HOME/bin` (after path normalisation), which is where
`tools/install.sh` puts the binary by default. Anything else — a container image, an install
with `--dir` somewhere else, a binary copied by hand, a build tree — gets the manual command
instead of a button. That narrowing is deliberate: matching more eagerly means running an
installer against a directory nobody put the binary in.

**A build that is not a release does not check at all.** `edc_version()` returns the commit
hash for a development build, `edc_semver_is_valid()` rejects it, and the check is skipped with
a log line saying so. Only a tagged build can tell whether a release is newer. This is why the
banner cannot be observed during development without `--update-check --current <ver>`.

**The check is silent on every failure.** Offline, DNS, a TLS error, a response body over
1 MiB, malformed JSON, and the unauthenticated API's **60 requests/hour/IP** limit (a 403) all
produce the same thing: no banner, no message, one `log_debug` line that never reaches the log
file. That is the intended behaviour — a background check has nothing to tell a user that they
could act on — but it means "no banner" is not evidence that the check ran.

**Turning the check off also disables `/update`.** With `update_check: false` no check runs, so
no version is ever on offer, so `/update` always answers "No update available". The network is
completely quiet, which is what the setting promises, at the cost of the manual path through the
command. To update with the check off, use the command the installer prints.

**Once per launch, not once per session.** Roughly 10s after startup, with a 10s timeout. A
session left open for days never looks again.

**No automatic restart.** The new binary is in place; quitting and starting again is the user's
step. An automatic restart would interrupt whatever task is running.

**No downgrade, ever.** The database migrates forward on the first start of the new build, and
an older binary refuses it afterwards. The confirmation page says this before the button, and it
is the reason an update is not something to click casually.

**Abandoning mid-upgrade can leave the install half-done.** Esc while it runs kills the
installer; whether the binary was replaced depends on how far it had got. The confirmation page
says so before the user starts, not after. One rough edge: Esc during the *download* stage
cannot interrupt the fetch (it has no cancellation hook), so the interface waits for it to
return — up to its 15s timeout — before the process is torn down. During the install itself the
kill is immediate.

**One upgrade at a time**, and each platform needs its own shell: `sh` plus `curl` or `wget` on
POSIX (the installer's own requirement), `powershell` on Windows (`install.ps1` then uses
`Invoke-WebRequest`, `tar` and `Get-FileHash`, all built in).

**Two platforms have no build at all**, and both installers refuse them explicitly rather than
installing something that will not run:

| Platform | Why |
|---|---|
| macOS x86_64 (Intel) | No target in the release matrix — only `edgeclaw_aarch64_macos` is built and published. |
| Windows arm64 | Same: only `edgeclaw_amd64_windows`. `PROCESSOR_ARCHITECTURE` must be `AMD64`. |

## Not verified

Stated plainly, because "it compiles" is not evidence:

| Area | State |
|---|---|
| Line coverage | **63.3%** — what [Codecov](https://app.codecov.io/github/edgeclaw-chat/edgeclaw) reports for `dev` at `e9a7fbf`, over 26348 instrumented lines across 137 files, re-measured 2026-11-12. It counts a line whose branches were not all taken as **not** covered, which is why it reads lower than the ~81% an lcov `LH/LF` total gives for the same run (81.2%, 21849/26899); the difference is the partially-executed lines. For reference, the lcov report from that run also measures functions 91.0% (1888/2074) and branches 58.3% (15475/26524); Codecov reports neither. All of these are measured with the three device tests excluded, the same way CI measures them. The counts move by a line or two between runs, because a few paths are timing-dependent — the percentage does not. The remaining gap is concentrated in paths that are hard to force deterministically: allocation failures, write errors at an exact moment, disk-full, sandbox denials. The LLM 4xx/5xx and HTTPS transport failures are covered, so are the first-run wizard, the frontend's C surface, `exec_shell`'s timeout/abort paths and the store's id-collision / measured-flush paths. |
| Windows | Builds in CI, and needs **no bash** to build: every `genrule` carries a `cmd_bat`, so Bazel runs those through `cmd.exe` (`tools/check_genrule_portability.py` enforces it). The PPPoS transport and the kernel sandbox do not exist there. The installer (`tools/install.ps1`) is exercised in CI under `pwsh` on Linux against a local TLS fixture server, and **not on real Windows hardware** for this release — so its `Invoke-WebRequest`/`tar`/`Get-FileHash` path is asserted, not observed. The owner-only protections (`~/.edgeclaw` and the workspace at `0700`, `config.json` and the database at `0600`) are **not implemented on Windows**: `edc_file_restrict_owner()` is a documented no-op there rather than a fabricated ACL, so those files keep the inherited ACL. |
| armv7 | Built locally (cross) and **run under `qemu-arm`**: the released shape of the binary starts and reports `platform linux/arm`. Not run on real armv7 hardware, and the Android device agent is compiled only. |
| Real-model behaviour | The suite drives a fake LLM. Prompt and tool-choice quality is not asserted anywhere. |
| macOS | Builds in CI (aarch64). Seatbelt sandbox paths are compiled, not exercised. The owner-only modes and the credential deny list added for this release are POSIX-guarded and were verified on Linux only; whether Seatbelt honours the same deny masks is not asserted. |
| Updating | The check and the installer are driven end to end against a local TLS server (`tests/test_update_check.c`), and the real endpoint was exercised once by hand. What is **not** asserted anywhere: that a real release archive upgrades a real installation, that the installer succeeds on a real user's machine, and that the banner/dialog path is reached in a live session (it needs a published release and a version that parses — neither exists during development). |

## Known sharp edges

Not "unverified" — verified, and wrong in a narrow case:

- **`--update-check` answers a question the app cannot otherwise be observed asking.** The
  release check is silent by design (see "Updating" above), so "no banner" never distinguishes
  "nothing newer" from "the check never ran". `edgeclaw --update-check [--pre] [--current <ver>]`
  runs it synchronously and prints the answer, which is the only way to exercise the network
  path without publishing a release. `--current` exists because a development build has no
  version to compare; it affects that diagnostic only.
  **A failed check and an up-to-date install print differently.** "up to date", "could not
  check: HTTP 403 (GitHub's unauthenticated rate limit, 60 requests/hour per IP)", "could not
  check: no response before the deadline" are three separate lines, because collapsing them
  was the command's own worst failure: it exists to tell those apart. The exit status stays 0
  in every case -- this diagnoses the updater, so a non-zero status would look like a failure
  of the thing being diagnosed. Read the text, not `$?`. The launch-time check is still silent
  to the user; only its log line names the reason.

- **An update proves the bytes arrived intact, not who sent them.** The installer compares
  each archive against the `sha256sums.txt` published next to it, which catches a truncated
  download, a wrong-platform asset, a stale mirror and an archive swapped on the wire. Both
  files come from the same place, so it does not prove authorship: whoever can replace one can
  replace the other. The boundary is TLS to GitHub plus GitHub's access control over the
  repository and its releases — the same boundary Homebrew formulae and crates.io rely on.
  Release signing was implemented, weighed and removed: it defends against a replaced archive
  rather than against the likelier failure (a poisoned dependency or a compromised maintainer
  account), and it would put the key on the same pipeline it is meant to protect. See
  `docs/auto-update.zh.md` §3.2. Treat "it came from the release page over TLS" as the claim,
  not "the author definitely built this".

- **The workspace is `0700`, so a second uid cannot read it.** The transcripts and the
  session database are the user's private data, and the directory mode is what keeps them
  private. The cost is that an install shared between accounts — `sudo` as another user, a
  bind mount whose owner does not match, a container running as a different uid — fails to
  read or write it. Tighten the ownership to the running uid rather than widening the mode.

- **The built-in deny list comes back.** Entries it seeds (the app's own key material, the
  conventional credential dotfiles, `/etc/shadow` and friends) are re-checked on every
  launch, so one the user deleted is added again. That is the deliberate direction: an
  unprotected secret file fails silently, while a re-added deny entry shows up in
  `/sandbox list` and can be removed again. To keep something readable on purpose, name a
  more specific path or turn the sandbox off.

- **`/encrypt` needs a terminal.** It prompts for the passphrase, so it cannot be used from
  a script or over a non-tty stdin — the same reason the first-run prompt needs one. Both
  have the `EDGECLAW_MASTER_KEY` route as the headless alternative, which does not need
  `/encrypt` at all.

- **Tool-output redaction is a backstop, not a boundary.** It recognises PEM private-key
  blocks, well-known token prefixes and credential-shaped assignments. A secret in a shape
  it does not know passes through untouched. The deny list is the mechanism that keeps a
  file from being read at all; this only limits what a read that already happened can leak.

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

