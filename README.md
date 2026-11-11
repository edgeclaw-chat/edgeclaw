<div align="center">
  <h1>EdgeClaw</h1>
  <p><em>A personal AI assistant that fits in one terminal — and runs many tasks at once.</em></p>
  <p>
    <img src="https://img.shields.io/badge/language-C-blue" alt="Language">
    <img src="https://img.shields.io/github/actions/workflow/status/edgeclaw-chat/edgeclaw/build.yml?branch=dev&amp;label=build" alt="Build">
    <img src="https://img.shields.io/github/actions/workflow/status/edgeclaw-chat/edgeclaw/tests.yml?branch=dev&amp;label=tests" alt="Tests">
    <a href="https://codecov.io/gh/edgeclaw-chat/edgeclaw"><img src="https://codecov.io/gh/edgeclaw-chat/edgeclaw/branch/master/graph/badge.svg" alt="Coverage"></a>
    <img src="https://img.shields.io/badge/license-MIT-green" alt="License">
  </p>
</div>

EdgeClaw is a self-contained terminal agent. You talk to a model that can read and write
files, run shell commands and schedule jobs — and instead of one
long conversation, every message becomes a **task** you can watch, queue, branch and
combine from the same input box. It ships as a single static binary (no runtime, no
package manager) with a full-screen TUI.

---

## What you get

**One inbox, many tasks.** Type a message and it becomes a task in the group in front.
Type another while the first is still running and it queues behind it — or send it to the
background with `&`, to a fresh conversation with `+`, or into a dependency chain with
`|` and `;`. Every task gets a state, an output and its own line in the sidebar.

**Conversations you can isolate and branch.** A *group* is one conversation; a *session*
is a directory + a set of groups; `/task fork` starts a child that inherits the parent's
memory and conversation but leads a group of its own.

**A shell vocabulary — on by default.** Turn it off with `/shell off` and every non-`/`
line becomes one task, verbatim — nothing is ever half-parsed. Left on (the default),
`"a" | "b"`, `"a" ; "b"`, `"task" &`, `"task" > out.md` compose tasks the way a
pipeline composes commands, while a line that begins with no quote and no sigil is still
one plain task description.

**`@file` references.** `explain @src/main.c` or `@src/main.c#40-80`. The file is read
through the sandbox, anchored at the session's directory, capped (2000 lines / 64 KB per
file), and handed to the model exactly the way the `read_file` tool would hand it over.
Works with the shell switch on or off.

**Commands and templates.** `/task`, `/session`, `/group`, `/model`, `/sandbox`, `/shell`
… plus `/prompt` and `/skill`, which run a template (or a `SKILL.md`) as one task, with
`$1 … $N` / `$ARGUMENTS` substituted. Prompt and skill directories are read from
`.edgeclaw`, `.claude`, `.agents`, `.opencode` and `.pi` roots, so material already laid
out that way works unchanged.

**Memory that survives.** Each group folds its finished tasks into a summary; each
session curates durable entries you can search (`/session memo`). Restart and the
conversation continues instead of starting over.

**A sandbox you can see.** Read-only / read-write / denied path lists, shown by
`/sandbox list` and enforced on every file and shell operation. `--no-sandbox` is there
for the day it gets in your way.

**Tools.** 13 of them: `read_file`, `write_file`, `edit_file`, `list_dir`, `exec_shell`,
`grep`, `find`, `crontab`, `todowrite`, `memory_write|list|supersede|delete`.

---

## Quick start

### Pre-built binary

Tarballs live on the [Releases](https://github.com/edgeclaw-chat/edgeclaw/releases) page.
Each holds one self-contained binary with the default workspace embedded — it is
released to `~/.edgeclaw/` on first run.

The installer picks the right build for this machine, resolves the newest release, and
puts the binary in `~/.local/bin`:

```bash
curl -fsSL https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/master/tools/install.sh | sh
```

It takes `--pre` when you want the newest pre-release (every release so far is one, so
without it the script stops and says so), `--tag <tag>` to pin an exact release,
`--musl` for the musl build on Linux, and `--dir <path>` to install somewhere else.
Read it first if you prefer: `tools/install.sh` — it is POSIX `sh` with no dependencies
beyond `curl` or `wget`.

#### Windows

Windows has its own installer, and it needs nothing beyond what the OS ships with
(`Invoke-WebRequest`, `tar`, `Get-FileHash` and .NET's ECDsa) — **no curl, no bash, no
Git for Windows**:

```powershell
iwr -UseBasicParsing https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/master/tools/install.ps1 `
    -OutFile "$env:TEMP\edgeclaw-install.ps1"
& "$env:TEMP\edgeclaw-install.ps1"
```

It installs to `%USERPROFILE%\.local\bin` and takes the same `-Tag`, `-Pre`, `-Dir` and
`-DryRun` options. Two steps rather than one pipe because the script takes arguments, and
piping it into `Invoke-Expression` would lose them.

#### No curl, no wget, or a hand-installed binary

Each archive is one self-contained binary — there is nothing to configure and no runtime to
install. Download the one for this machine from the
[Releases](https://github.com/edgeclaw-chat/edgeclaw/releases) page, unpack it with any tool
the machine has, and run it.

That is also the answer for an air-gapped machine, and for a copy that was installed without
the installer: a program that cannot tell how it got there will not try to upgrade itself —
it prints this same instruction rather than guessing.

Every archive carries the tag in its name — `edgeclaw_amd64_linux_v1.0.0-rc.1.tar.gz` —
so a file kept for months still says what it is without being run. There is deliberately
no version-less name, which is why `releases/latest/download/<name>` does not work and a
fixed download URL cannot be written down here: build it from the tag instead.

```bash
TAG=v1.0.0-rc.1   # replace with the tag shown on the Releases page
curl -LO "https://github.com/edgeclaw-chat/edgeclaw/releases/download/${TAG}/edgeclaw_amd64_linux_${TAG}.tar.gz"
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

### From source

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

### First run

No config file? One is written with defaults, and `/connect` opens the setup dialog
(provider, API URL, model, key) at any time. The only prompt is the master passphrase —
set `EDGECLAW_MASTER_KEY` to skip it. Type `/help` for the command list.

---

## Input at a glance

The first character decides what the line is. That is the whole rule.

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
> "generate the report" > report.md         # write the task's output to a file
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
* **Session** — a working directory plus its groups. `/session new [name] [dir]` binds a
  new one; conversations are stored under `workspace/<session_id>/`.

`/task resume <id>` picks up where a stopped, failed or interrupted task left off — it
re-derives whether the task was waiting on dependencies, mid-tool-call, or waiting for the
model, and continues instead of starting over. The id is required (there is no batch form);
`/task list` shows the tasks, and `/task status <id>` also says *why* one ended.

---

## Commands

| Group | Commands |
|---|---|
| General | `/help` `/whoami` `/model` `/prompt` `/skill` `/reload` `/connect` `/sandbox` `/shell` |
| Session | `/session new · switch · list · info · status · save · stop · remove · rename · memo` |
| Tasks | `/task new · fork · list · status · output · history · fg · bg · stop · resume` |
| Groups | `/group list · info · history · fold` |
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

`~/.edgeclaw/config.json` (created on first run; `/connect` edits it):

```json
{
  "api_url": "https://api.openai.com/v1/chat/completions",
  "api_key": "",
  "model": "gpt-4o",
  "max_iter": -1,
  "context_window": 1048576,
  "tool_max_output_size": 10000,
  "shell_syntax": false,
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

Files live under `~/.edgeclaw/`: `config.json`, `workspace/<session_id>/history.jsonl`,
`workspace/skills/`, `workspace/prompts/`.

---

## Development

```bash
bazel test //tests:all        # 131 test targets (135 cc_test declared; see below)
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
