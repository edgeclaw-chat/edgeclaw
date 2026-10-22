# Building on Windows

Goal: `bazel build` works from **Windows Terminal (PowerShell)** and from **Git Bash**,
on any machine, and never depends on WSL. One command does the setup:

```powershell
powershell -ExecutionPolicy Bypass -File tools\windows\setup-build-env.ps1
bazel shutdown          # a running bazel server keeps the environment it started with
```

Open a new terminal first (existing ones keep their old environment).

Nothing in this repository names a location. The script discovers what it needs on the
machine it runs on; see [Where the tools come from](#where-the-tools-come-from).

## Why any setup is needed

The flex/bison genrules in `//src/shell` (`lex_c`, `parse_c`) are bash commands, so Bazel
has to pick a bash — and on Windows it picks one **from the environment**. That makes the
build depend on the terminal it was started from. Measured with `bazel aquery`:

| Started from | genrule shell Bazel picks | flex / bison |
|---|---|---|
| Windows Terminal (PowerShell) | `C:\Windows\System32\bash.exe` — **WSL** | whatever that WSL distro has in `/usr/bin` |
| Git Bash | `<Git>\usr\bin\bash.exe` — Git for Windows | none: Git ships neither |

So "it builds in Windows Terminal" was really "this machine happens to have a WSL distro
that ships flex and bison", and "it fails in Git Bash" was "Git's bash has neither, and
nothing on PATH provides them".

There is a second, independent Git Bash trap: the MSYS runtime rewrites arguments that
look like paths, and it eats one slash off single-component Bazel labels
(`//tests:all` -> `/tests:all`, `//...` -> `/...`), which Bazel then rejects with
`invalid package name '/tests:all': package names may not start with '/'`. Labels with
two or more components (`//src/tui:edgeclaw`) are left alone, which is why it fails
selectively and looks random.

## What the script sets (user scope, HKCU only)

| Setting | Value | Why |
|---|---|---|
| `BAZEL_SH` | the `bash.exe` of the `git` found on PATH | Pins the genrule shell in *every* terminal, so WSL is never consulted again. Discovered per machine, never assumed. |
| `PATH` (user) | `-ToolsDir`, **only if** flex/bison are not already reachable | Makes them visible to the genrule. There is no default directory: if they are already on PATH, nothing is added. |
| `~/.bashrc` | `export MSYS2_ARG_CONV_EXCL='//'` | Stops the label rewriting; scoped to `//` so ordinary path arguments still convert. |

`BAZEL_SH` is a documented Bazel variable, so none of this needs a change to `.bazelrc`
(shared with CI and the other platforms) and no machine-specific path enters the repo.

The script also refuses a bash whose `uname -sr` says `microsoft`: a WSL distro may well
have flex and bison, but that is the dependency this setup exists to remove.

## Where the tools come from

**flex and bison are host tools** — that is a deliberate, documented choice
(`_HOST_TOOLS_CHECK` in `//src/shell:BUILD`), and it is
the same contract the Linux and macOS builds use: the tools are found on `PATH`, like
`gcc` or `python`. A build that needs them can therefore never be self-contained; what it
*can* be is location-independent, which is why nothing here hardcodes a directory.

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

Either way, **GNU Bison 3.0+** is required: the grammar uses `%define api.value.type` /
`%define parse.error` and the genrule passes `-Werror=conflicts-*`. macOS' system bison
is 2.3 from 2006; Git for Windows ships none. `_HOST_TOOLS_CHECK` turns all of those into
one readable error instead of a raw tool complaint.

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
# only if flex/bison are not on PATH yet:
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
