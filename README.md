<div align="center">
  <h1>EdgeClaw</h1>
  <p><em>A personal AI assistant that fits in one terminal — and runs many tasks at once.</em></p>
  <p>
    <img src="https://img.shields.io/badge/language-C%2FC%2B%2B-00599C?logo=c%2B%2B&amp;logoColor=white" alt="Language: C/C++">
    <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-1f2937" alt="Platforms: Linux, macOS, Windows">
    <img src="https://img.shields.io/badge/binary-static%20single%20file-orange" alt="Static single-file binary">
    <a href="https://github.com/edgeclaw-chat/edgeclaw/releases"><img src="https://img.shields.io/github/v/release/edgeclaw-chat/edgeclaw?include_prereleases" alt="Latest release"></a>
    <img src="https://img.shields.io/badge/license-MIT-green" alt="License">
  </p>
  <p>
    <img src="https://img.shields.io/github/actions/workflow/status/edgeclaw-chat/edgeclaw/build.yml?branch=dev&amp;label=build" alt="Build">
    <img src="https://img.shields.io/github/actions/workflow/status/edgeclaw-chat/edgeclaw/tests.yml?branch=dev&amp;label=tests" alt="Tests">
    <a href="https://codecov.io/gh/edgeclaw-chat/edgeclaw"><img src="https://codecov.io/gh/edgeclaw-chat/edgeclaw/branch/master/graph/badge.svg" alt="Coverage"></a>
    <a href="docs/edgeclaw.md"><img src="https://img.shields.io/badge/docs-manual-blue?logo=bookstack&amp;logoColor=white" alt="Documentation"></a>
  </p>
</div>

EdgeClaw is an open-source terminal AI agent for working with local files, shell commands
and scheduled jobs. Every message becomes a **task**: queue work, run it in the background,
connect tasks with pipes, or fork a conversation without leaving the input box.

It ships as a single binary with a full-screen TUI and an embedded workspace. Release
binaries run without Node.js, Python, or a separate application runtime.

[Quick start](#quick-start) · [Try a workflow](#try-a-workflow) ·
[Installation options](#installation-options) · [Commands](#commands) ·
[Limitations](docs/limitations.md) · [Contributing](CONTRIBUTING.md)

## What you get

- **Task control in one terminal.** Watch task states and results in the sidebar. Continue
  typing while work runs, queue it in the same conversation, or start a separate group.
- **Pipes, background jobs and forks.** Use `|` to pass one task's output to the next, `;`
  to run in sequence, `&` for background work, and `/task fork` to branch with context.
- **Local tools and file references.** Read, write and edit files; search, run shell
  commands and schedule jobs. Attach a file or a line range with `@path` or `@path#40-80`.
- **Saved conversations and memory.** Groups share conversation history; sessions bind
  groups to a working directory. Save, restart and resume work, with group summaries and
  searchable session memory.
- **Reusable prompts and skills.** Run templates with `/prompt` and `/skill`. Resource
  directories include `.edgeclaw`, `.claude`, `.agents`, `.opencode` and `.pi`.
- **Visible access controls.** Inspect writable, read-only and denied paths with
  `/sandbox list`. Enforcement and platform differences are documented in
  [Limitations](docs/limitations.md#the-sandbox).

## Before you start

- An API key, an **OpenAI-compatible Chat Completions endpoint**, and a model that supports
  tool calling. Built-in provider/model entries simplify configuration; they do not mean
  every listed provider and model has been verified. Native APIs with a different protocol
  need a compatible endpoint or gateway.
- A supported machine: Linux x86_64, aarch64 or armv7; macOS Apple Silicon; or Windows
  x86_64. See the [release archive table](#manual-download) for the exact build names.
- A terminal and **local filesystem** for the state workspace. One EdgeClaw process can
  open a state workspace at a time; NFS, SMB, FUSE and virtiofs are refused. See
  [persisted-state limits](docs/limitations.md#persisted-state) for details.

## Quick start

### 1. Install

**Linux and macOS Apple Silicon:**

```bash
curl -fsSL https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/master/tools/install.sh | sh
export PATH="$HOME/.local/bin:$PATH"
edgeclaw --version
```

The installer selects the newest stable release for this machine, checks the downloaded
archive against `sha256sums.txt`, and installs to `~/.local/bin`. Add that directory to
your shell's startup configuration to keep it on `PATH` in future terminals.

**Windows x86_64 — run in PowerShell:**

```powershell
iwr -UseBasicParsing https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/master/tools/install.ps1 `
    -OutFile "$env:TEMP\edgeclaw-install.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "$env:TEMP\edgeclaw-install.ps1"
& "$env:USERPROFILE\.local\bin\edgeclaw.exe" --version
```

The Windows installer uses the OS-provided download, archive and hashing tools; no bash
or Git for Windows is required. Follow its `PATH` hint, or launch the executable by its
full path. [Other Windows shells](#other-windows-shells) and
[manual downloads](#manual-download) are covered below.

### 2. Open a directory and configure a model

Start in a project directory, or create a scratch directory for the example below:

```bash
mkdir -p edgeclaw-demo
cd edgeclaw-demo
edgeclaw
```

On Windows, create/open the directory in PowerShell and run `edgeclaw` if it is on `PATH`,
or use `& "$env:USERPROFILE\.local\bin\edgeclaw.exe"`.

On first run:

1. Choose and confirm a passphrase to encrypt the API key. You enter it again at launch.
   Choosing an empty passphrase stores the key in plaintext; the prompt explains the choice.
2. Select a provider, enter its API key, and choose a model. For a gateway or self-hosted
   service, choose **Custom endpoint (OpenAI-compatible)** and supply its base URL, key,
   model ID and context window.
3. The workspace opens. Type `/help` to see the commands; `/connect` reopens setup at any
   time. Setup saves the settings; your first message makes the actual model request.

### 3. Run your first task

Enter these lines **one at a time inside EdgeClaw**, in the scratch directory:

```text
!echo Audience: terminal developers. Goal: prepare a public demo. > brief.md
Read @brief.md and create release-checklist.md with six actionable checkboxes.
/task list
```

The first line creates a sample file through the local shell without a model call.
The second attaches that file to a real model task and asks the agent to write a checklist.
The task list shows its state. Open `release-checklist.md` in your editor to inspect the
generated file.

## Try a workflow

### Run independent work in the background

Continue in the same scratch directory. Enable shell syntax explicitly, then start two
separate conversations without blocking the input box:

```text
/shell on
+"Read @brief.md and suggest three launch risks." &
+"Read @brief.md and write a two-sentence launch announcement." &
/task list
```

Use `/task output <id>` to read a result, or `/task fg <id>` to bring a task's group to
the foreground. Replace `<id>` with the number shown by `/task list`.

### Pass a result to the next task

```text
"Summarize @brief.md in three bullets." | "Turn the previous output into a short announcement."
```

The second task waits for the first and receives its output. Use `;` instead of `|` when
you only need ordering, without passing the output.

### Save and pick up later

```text
/session save
```

Press Ctrl+C twice to quit, then launch EdgeClaw again from the same directory. Use
`/task list` and `/task output <id>` to inspect the saved work. For a task that was still
running when you quit, `/task resume <id>` continues the interrupted task.

---

## Installation options

The POSIX installer supports:

| Option | Effect |
|---|---|
| `--pre` | Include pre-releases when choosing the newest release. |
| `--tag <tag>` | Install a specific published release, including a pre-release. |
| `--musl` | Select a Linux musl build. |
| `--dir <path>` | Install somewhere other than `~/.local/bin`. |
| `--dry-run` | Resolve and print the target and URL without downloading the archive. |

Pass arguments through the pipe with `sh -s --`, for example:

```bash
curl -fsSL https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/master/tools/install.sh | sh -s -- --pre
```

Read [tools/install.sh](tools/install.sh) before running it if you prefer. It uses POSIX
`sh`, `curl` or `wget`, `tar`, standard shell utilities, and `sha256sum` or `shasum`.
Release tags and platform assets are listed on the
[Releases](https://github.com/edgeclaw-chat/edgeclaw/releases) page.

### Other Windows shells

The quick-start Windows commands require PowerShell. Windows Terminal is a window, not a
shell: open a PowerShell tab, or use the command for the shell you already have open.

From `cmd`, as one line, with the alias spelled out because `cmd` has none:

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p=Join-Path $env:TEMP 'edgeclaw-install.ps1'; Invoke-WebRequest -UseBasicParsing https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/master/tools/install.ps1 -OutFile $p; & $p"
```

From Git Bash, the same thing with bash's own `$TEMP` — and `cygpath`, because PowerShell
wants a Windows path:

```bash
curl -fsSL https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/master/tools/install.ps1 -o "$TEMP/edgeclaw-install.ps1" &&
    powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$TEMP/edgeclaw-install.ps1")"
```

It installs to `%USERPROFILE%\.local\bin` and takes the same `-Tag`, `-Pre`, `-Dir` and
`-DryRun` options. Two steps rather than one pipe because the script takes arguments, and
piping it into `Invoke-Expression` would lose them.

`-ExecutionPolicy Bypass` is on the line that runs the script because the default policy is
`Restricted`, which refuses to load any `.ps1` — the shorter `& "$env:TEMP\edgeclaw-install.ps1"`
dies with "running scripts is disabled on this system" before the script starts. `Bypass` is
scoped to that one process and changes no machine-wide setting; it is also how the app runs
this same installer when it upgrades itself.

### Manual download

Each archive contains one self-contained binary. No additional runtime is needed;
provider setup still happens on first run. Download the one for this machine from the
[Releases](https://github.com/edgeclaw-chat/edgeclaw/releases) page, unpack it with any tool
the machine has, and run it.

You can transfer the archive to another machine for offline installation; model access
still depends on the configured endpoint. A manually installed copy gets manual update
instructions when its installation location is not recognized.

Every archive carries the tag in its name — `edgeclaw_amd64_linux_<tag>.tar.gz` —
so a file kept for months still says what it is without being run. There is deliberately
no version-less name, which is why a fixed download URL cannot be written down here: the
name needs a tag that does not exist yet. The installer reads the release feed to find
the current tag and builds the filename from it, without touching the rate-limited API.

```bash
TAG='<release-tag>'   # replace with a published tag from the Releases page
curl -fLO "https://github.com/edgeclaw-chat/edgeclaw/releases/download/${TAG}/edgeclaw_amd64_linux_${TAG}.tar.gz"
tar -xzf "edgeclaw_amd64_linux_${TAG}.tar.gz" && cd edgeclaw_amd64_linux && ./edgeclaw
```

| Platform | Tarball (`<tag>` is the release tag) | Binary inside |
|----------|---------------------------------------|---------------|
| Linux x86_64 | `edgeclaw_amd64_linux_<tag>.tar.gz` | `edgeclaw` |
| Linux aarch64 | `edgeclaw_aarch64_linux_<tag>.tar.gz` | `edgeclaw` |
| Linux armv7 | `edgeclaw_arm_linux_<tag>.tar.gz` | `edgeclaw` |
| macOS aarch64 | `edgeclaw_aarch64_macos_<tag>.tar.gz` | `edgeclaw` |
| Windows x86_64 | `edgeclaw_amd64_windows_<tag>.tar.gz` | `edgeclaw.exe` |

The three Linux targets are also published built against musl libc as
`edgeclaw_<arch>_linux_musl_<tag>.tar.gz` — the same program, just a different libc
linked in.

Use the archive for your platform and verify it against the release's
`sha256sums.txt`. macOS Intel and Windows arm64 are not in the current release matrix.
Build availability and runtime verification are listed separately in
[Limitations](docs/limitations.md#not-verified).

### Updating

When a newer release is detected, the TUI shows a banner. Use `/update` to inspect and
confirm the upgrade, then quit and restart to use the new binary. Automatic installation
is offered for recognized installer locations; other installations get manual instructions.

To check explicitly from your terminal:

```bash
edgeclaw --update-check
```

Read the reported result: the diagnostic distinguishes an up-to-date install from a failed
check. State schemas migrate forward, so an older binary may refuse the updated workspace.
See [update limits](docs/limitations.md#updating) before relying on a downgrade.

---

## Input at a glance

With shell syntax enabled (`/shell on`), the first character decides how input is read.
Plain messages, `/` commands, `!` local shell commands, and `@file` references also work
with shell syntax disabled.

| Line starts with | It is | Example |
|---|---|---|
| anything else | **one task**, verbatim | `explain src/edc_interp.c` |
| `/` | **a command** — always; an unknown one is an error, not a message | `/task list` |
| `!` | **a local shell command**, no model call, no tokens | `!git status` |
| `+` | a task in a **new group** (shell syntax) | `+"review the last commits"` |
| `"` or `'` | a **shell line**: pipes, sequences, redirects | `"scan src/" \| "fix the top 3"` |

```
> explain @src/tui/edc_input.hpp            # a file reference, no shell syntax needed
> "scan for bugs" | "write a fix plan"      # two tasks; the second sees the first's output
> "run the tests" ; "summarize failures"    # sequential, no output injection
> "run the full suite" &                    # background: the prompt returns at once
> "generate the report" > "report.md"       # write the task's output to a file
> /task list                                # everything that is running, queued or done
```

Quoting is bash's: `"…"` interpolates `{{…}}`, `'…'` is raw, and the two **triple forms**
`"""…"""` / `'''…'''` span lines with bare quotes allowed inside — the shape pasted text
wants (`'''…'''` is `$'…'` block-shaped, so `\n` there is a newline). Full table:
`docs/edgeclaw.md`, *Shell Architecture*.

While you type a `/` command the completion list sits just above the input: ↑/↓ to move,
Tab to complete, Esc to dismiss. Enter sends what you typed — unless you have walked the
list with ↑/↓, in which case Enter picks the highlighted row instead (Tab does the same,
and the next Enter sends). Esc alone stops the foreground task; Ctrl+C clears the input
(twice to quit).

↑/↓ recall earlier messages, but only ones that **start with what you have already typed**
(an empty line recalls everything). Type `task ` and ↑ walks the `task …` lines; if nothing
matches, ↑ keeps moving the caret instead. The draft you were typing comes back on ↓ past
the newest match.

`@path#12-20` attaches just those lines. `\@` is a literal `@`. A missing or binary file
does not swallow your message: the task runs and says what happened.

---

## Tasks, groups, sessions

* **Task** — one input and the conversation it produced. States: `IDLE → WAITING_DEPS →
  LLM_PROCESSING ⇄ TOOL_CALLING → COMPLETED / CANCELLED / ERROR`, plus `INTERRUPTED`
  (what every unfinished task becomes on exit).
* **Group** — one conversation. Tasks in a group share history; only the group's active
  task writes to the screen. `|` exchanges output *between* tasks, `;` only waits.
* **Session** — a working directory plus its groups. `/session new [name] <dir>` binds a
  new one; task and session state is stored in `~/.edgeclaw/workspace/edgeclaw.db`.

`/task resume <id>` picks up where a stopped, failed or interrupted task left off — it
re-derives whether the task was waiting on dependencies, mid-tool-call, or waiting for the
model, and continues instead of starting over. The id is required (there is no batch form);
`/task list` shows the tasks, and `/task status <id>` also says *why* one ended.

---

## Commands

| Group | Commands |
|---|---|
| General | `/help` `/whoami` `/model` `/prompt` `/skill` `/reload` `/connect` `/sandbox` `/shell` `/update` |
| Session | `/session new · switch · list · info · status · save · stop · remove · rename · memo` |
| Tasks | `/task new · fork · list · status · output · history · fg · bg · stop · resume` |
| Groups | `/group list · info · history · fold · archive · restore · rm` (archive retires a group out of the sidebar without losing its memory) |
| Scheduling | `/jobs list · run <id> · rm <id>` |

Common options: `--bg` (same group, background), `--new-group`, `-d <id>` (wait for a task
and inject its output), `-g <group>`. A shell line says background with `&`, not `--bg`.

Full reference: `docs/edgeclaw.md` (or `man docs/edgeclaw.1`). What it deliberately does
*not* do, and where the rough edges are: [`docs/limitations.md`](docs/limitations.md) —
worth reading before you file a bug.

---

## How it works

Input arrives from the TUI (or a scheduled job) and never reaches the model directly —
it becomes a **task**, and the task's turn is assembled per request:

```
user input → lexer → classify ─┬─ shell line → parser → AST ─┬→ passes → interpreter → task
                               └─ message ───────────────────┘
```

* **The AST is the pipeline's IR.** A message gets a node too (the grammar itself only
  produces shell trees), so both branches meet at the same place.
* **Passes** run between parsing and interpretation and only rewrite the IR. Today there
  is one: `@file` expands a reference into a read-tool-shaped block, delivered as its own
  message next to what you typed — so `raw_input`, the transcript and `/task history` keep
  showing your text, and a file's body is never rewritten by variable expansion.
* **Actors** carry the concurrency: scheduler, LLM, toolbox, memory and channel each have
  a mailbox on one shared `libuv` loop, so a background task's HTTPS call never blocks the
  input box. Threads only appear where they must (HTTPS).
* **Task states drive everything else** — dependencies (`JOIN` injects output, `SEQUENCE`
  only waits, failures cascade), group folds, and snapshot rendering in the TUI.

```
src/
  tui/        FTXUI frontend (C++): input box, chat log, sidebar, completion popup
  app/        the C API the frontend talks to (edc_app_*)
  shell/      lexer → classifier → parser → passes → interpreter
  kernel/     scheduler, tasks, turns, sessions, groups, context (prompts/skills/memory)
  toolbox/    the 13 tools
  actor/      mailboxes, LLM request/response, tool dispatch
  common/     config, containers, file/path utils, HTTPS client
  sandbox/    path validation + optional kernel-enforced isolation
tests/        bazel test //tests:all
docs/         manual, input pipeline, design notes
```

---

## Configuration

`~/.edgeclaw/config.json` is created on first run; `/connect` updates the model settings.
The example below illustrates the fields on Linux. Paths and generated defaults vary by
platform; use the generated file as the starting point for your own settings:

```json
{
  "api_url": "https://api.openai.com/v1/chat/completions",
  "api_key": "",
  "model": "gpt-4o",
  "max_iter": -1,
  "context_window": 1048576,
  "tool_max_output_size": 10000,
  "shell_syntax": true,
  "sandbox": {
    "enable": true,
    "rw_paths": ["<home>/.edgeclaw/workspace", "/tmp", "/dev", "<home>/.cache"],
    "ro_paths": ["/usr", "/lib", "/bin", "/etc", "/proc", "/sys", "/run", "/opt"],
    "deny_paths": []
  },
  "channel": { "type": "adapter" }
}
```

`max_iter: -1` means "keep going until the model answers"; a positive number caps the
model/tool rounds. `sandbox.deny_paths` wins over `ro_paths` / `rw_paths`.

`/opt` is in the read-only list because that is where a toolchain installed outside the
package manager lives -- on Linux the musl cross compilers the static release binaries are
built with. Editing the sandbox lists of a running app does not reach a `bazel` server that
is already up: it is a long-lived process and the kernel rules it was started under are
irreversible, so `bazel shutdown` before the next build.

Environment: `EDGECLAW_MASTER_KEY` (skip the passphrase prompt), `EDGECLAW_MODELS_CATALOG`,
`EDGECLAW_THEME`, `EDGECLAW_HTTPS_TIMEOUT_MS`, `EDGECLAW_SNAPSHOT_TRACE`.

Files live under `~/.edgeclaw/`: `config.json`, the `workspace/edgeclaw.db` state store,
`workspace/skills/`, `workspace/prompts/`, and the bundled user manual in `docs/`.

---

## Development

### Build from source

```bash
git clone https://github.com/edgeclaw-chat/edgeclaw.git && cd edgeclaw
bazel build //src/tui:edgeclaw          # the interactive TUI
bazel build //:edgeclaw                 # release tarball (same binary, embedded workspace)
bazel run   //src/tui:edgeclaw          # build and start
```

Requires Bazel plus a C/C++17 toolchain.

#### Windows (building from source)

Visual Studio's C++ workload (MSVC) plus Bazel — that is the compiler toolchain; there is
no MinGW involved, and **no bash**: every `genrule` carries a `cmd_bat`, so Bazel runs those
with `cmd.exe` instead of routing them through a POSIX shell.
[tools/check_genrule_portability.py](tools/check_genrule_portability.py) enforces that, and
runs in CI.

```powershell
bazel build //src/tui:edgeclaw
```

One optional step, for a different reason: Git Bash's MSYS runtime rewrites arguments that
look like Unix paths, so `//tests:all` can reach Bazel as `/tests:all` when a build is
started from that terminal. Pinning the environment removes the ambiguity — and also stops
Bazel picking the WSL `bash` out of `System32` for anything that still wants one:

```powershell
powershell -ExecutionPolicy Bypass -File tools\windows\setup-build-env.ps1
bazel shutdown
```

That script is no longer a prerequisite for building; it is there for the label-rewriting
trap and for a deterministic environment. It sets `BAZEL_SH` to the bash of the git on PATH
and `MSYS2_ARG_CONV_EXCL='//'`; no path is hardcoded, everything is discovered on the machine
that runs it. See [tools/windows/README.md](tools/windows/README.md).

**flex and bison are not build prerequisites.** Their output is committed
(`src/shell/edc_lex.c`, `edc_parse.tab.c`), so a plain build never runs them; the two
generator rules are tagged `manual`. You need them only if you change the grammar, and then
the entry point is `tools/regen_grammar.sh` — see [CONTRIBUTING.md](CONTRIBUTING.md).

### Verification and contributions

```bash
bazel test //tests:all        # run the non-manual test targets
bazel build //tests:all       # compiles the `manual` targets too -- see below
bazel build //...             # everything, including the release tarball
```

`bazel test` skips targets tagged `manual` (the ones needing network, a serial device or a
long run), so **they compile only under `bazel build //tests:all`**. CI runs both; run both
too, or those targets rot unnoticed — which is exactly what happened to two of them.

**Branches.** `dev` is the trunk: work lands there, and `master` is only ever a merge of
`dev` — never committed to directly. A release is a tag on `master`. See
[`docs/release-checklist.md`](docs/release-checklist.md) for what a release needs to check.

[`CONTRIBUTING.md`](CONTRIBUTING.md) has the build commands, the `manual`-target trap that
makes `bazel test` alone insufficient, and the conventions a patch should follow.
[`SECURITY.md`](SECURITY.md) says how to report a vulnerability privately, and — just as
usefully — what is a documented limitation rather than a security bug.

Conventions worth knowing before a patch:

* C11. State is owned by an explicit context (actor, session, task); the input parser's
  scanner is the one file-level exception, and it is single-threaded by design.
* One owner per decision — "how an input becomes a task" lives in the shell layer, "what a
  turn contains" in the kernel. New syntax goes in as a **pass**, not as a branch in the
  interpreter.
* Tests are per-behaviour, not per-file (`test_mention_pass`, `test_shell_job_control`,
  `test_llm_request`, …), and they drive the public entry points.

## License

MIT. See [LICENSE](LICENSE).
