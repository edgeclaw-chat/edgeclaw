# Building on Windows

Goal: `bazel build` works from **Windows Terminal (PowerShell)** and from **Git Bash**,
on any machine, and never depends on WSL.

**No bash is required to build.** Every `genrule` in the tree carries a `cmd_bat`, so on
Windows Bazel runs those with `cmd.exe` rather than routing them through a POSIX shell.
`tools/check_genrule_portability.py` enforces that (a rule without one would silently bring
the dependency back), and it runs in CI.

One command does the remaining setup, which is now optional:

```powershell
powershell -ExecutionPolicy Bypass -File tools\windows\setup-build-env.ps1
bazel shutdown          # a running bazel server keeps the environment it started with
```

Open a new terminal first (existing ones keep their old environment).

Nothing in this repository names a location. The script discovers what it needs on the
machine it runs on; see [flex and bison](#flex-and-bison-only-for-changing-the-grammar)
for the optional part.

## Why any setup is needed

Not for the genrules any more — those have `cmd_bat`. What remains is a Git Bash trap and
the value of a deterministic environment.

The trap: the MSYS runtime rewrites arguments that look like paths, and it eats one slash
off single-component Bazel labels (`//tests:all` -> `/tests:all`, `//...` -> `/...`), which
Bazel then rejects with `invalid package name '/tests:all': package names may not start
with '/'`. Labels with two or more components (`//src/tui:edgeclaw`) are left alone, which
is why it fails selectively and looks random. Setting `MSYS2_ARG_CONV_EXCL='//'` stops it.

The environment: pinning `BAZEL_SH` also settles which `bash` anything else that wants one
would get, instead of depending on which terminal you opened.

<details>
<summary>History: this used to be a build prerequisite</summary>

Bazel ran every `genrule` command through `bash`, and on Windows it picked one **from the
environment**. Measured with `bazel aquery` before `cmd_bat` existed:

| Started from | genrule shell Bazel picked |
|---|---|
| Windows Terminal (PowerShell) | `C:\Windows\System32\bash.exe` — **WSL** |
| Git Bash | `<Git>\usr\bin\bash.exe` — Git for Windows |

Neither was reliably present, so the build depended on the terminal it was started from.
One rule made it worse: `//:models_catalog_json` began `if [ -n '$(SRCS)' ]`, and a shell
conditional is the one construct that cannot be mirrored into `cmd_bat` without writing the
same logic twice — so that decision moved into `tools/gen_models.py`
(`--copy-if-present`), leaving a single tool invocation.

</details>

<details>
<summary>History: flex and bison were in the build path</summary>

The generator rules used to be on the default build path, so a build also needed flex and
bison on `PATH`. They are not any more — their output is committed and the rules are tagged
`manual` — see [flex and bison](#flex-and-bison-only-for-changing-the-grammar) below.

</details>

## What the script sets (user scope, HKCU only)

| Setting | Value | Why |
|---|---|---|
| `BAZEL_SH` | the `bash.exe` of the `git` found on PATH | Settles which bash anything outside a `cmd_bat` would get, so it never depends on which terminal opened. Genrules themselves no longer use it. Discovered per machine, never assumed. |
| `PATH` (user) | `-ToolsDir`, **only if** flex/bison are not already reachable | Optional, and only for regenerating the grammar (see below). There is no default directory: if they are already on PATH, nothing is added. |
| `~/.bashrc` | `export MSYS2_ARG_CONV_EXCL='//'` | Stops the label rewriting; scoped to `//` so ordinary path arguments still convert. |

`BAZEL_SH` is a documented Bazel variable, so none of this needs a change to `.bazelrc`
(shared with CI and the other platforms) and no machine-specific path enters the repo.

The script also refuses a bash whose `uname -sr` says `microsoft`: a WSL distro may well
have a bash, but depending on one is the thing this setup exists to remove.

## flex and bison: only for changing the grammar

**They are not build prerequisites.** `src/shell/edc_lex.c` and `edc_parse.tab.c` are
committed, `//src/shell:lex` compiles them directly, and the two generator rules
(`lex_c`, `parse_c`) are tagged `manual` — off every default build path. CI checks the
committed output is not stale (`tools/regen_grammar.sh --check`), so editing the grammar
without regenerating fails with the command to run rather than producing a silent mismatch.

So install them only if you intend to edit `edc_lex.l` or `edc_parse.y`. When you do, the
entry point is `tools/regen_grammar.sh`, never Bazel directly: it pins tool versions (Bison
3.8.x, Flex 2.6.x), normalises what the generators write, and refuses output containing an
absolute path — all of which is what lets `--check` compare bytes across machines.

Windows ships neither tool. Options, in order of least surprise:

1. **MSYS2** (`pacman -S flex m4 bison`) and put `<msys2>\usr\bin` on PATH. A
   bison installed inside its own MSYS2 root finds its skeleton files and `m4` by
   itself.
2. **A partial extraction** (only the `flex`/`bison`/`m4` packages, unpacked somewhere
   outside an MSYS2 root). Then bison's two compiled-in defaults point at the wrong
   place: `BISON_PKGDATADIR` becomes `/usr/share/bison` (no skeletons there) and `M4`
   becomes `/usr/bin/m4` (unset, bison dies with `m4 subprocess failed`). Fix it with a
   small `bison` wrapper that exports both and execs the real binary, e.g.

   ```sh
   #!/bin/sh
   export BISON_PKGDATADIR=/d/tools/msys/usr/share/bison
   export M4=/d/tools/msys/usr/bin/m4.exe
   exec /d/tools/msys/usr/bin/bison.exe "$@"
   ```

   and pass the directory holding that wrapper (and a matching `flex` wrapper) to
   `-ToolsDir`.

Either way, **GNU Bison 3.8.x** is required: the grammar uses `%define api.value.type` /
`%define parse.error`, and `regen_grammar.sh` refuses other versions because the version
string is embedded in the generated file's first line. macOS' system bison is 2.3 from
2006; Git for Windows ships none.

## Verifying

In **either** terminal, after `bazel shutdown`:

```bash
bazel aquery //src/shell:lex_c | grep "Command Line"
#   Command Line: <Git>\bin\bash.exe ...      <- not C:\Windows\System32\bash.exe
bazel build //src/tui:edgeclaw
```

In Git Bash, this must also work — it is the label that used to be rewritten:

```bash
bazel query //tests:all
```

`tools\windows\setup-build-env.ps1 -CheckOnly` reports what it would change (or
"Nothing to change") without writing anything.

## Manual equivalent

```powershell
# bash: the one belonging to the git on PATH
$git = (Get-Command git).Source
$bash = Join-Path (Split-Path (Split-Path $git -Parent) -Parent) 'bin\bash.exe'
[Environment]::SetEnvironmentVariable('BAZEL_SH', $bash, 'User')
# optional, only for regenerating the grammar (see above):
[Environment]::SetEnvironmentVariable('PATH', '<dir>;' + [Environment]::GetEnvironmentVariable('PATH','User'), 'User')
```
```bash
echo "export MSYS2_ARG_CONV_EXCL='//'" >> ~/.bashrc
```

and, for the current session only:

```bash
export MSYS2_ARG_CONV_EXCL='//'
export BAZEL_SH="$(cygpath -w "$(command -v git)" | sed 's#\\cmd\\git.exe##')/bin/bash.exe"   # or set it explicitly
bazel shutdown
```
