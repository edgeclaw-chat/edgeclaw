#!/usr/bin/env python3
"""Reject POSIX-only libc calls in files that do not pull in the compatibility shim.

    python3 tools/check_portability.py          # check src/
    python3 tools/check_portability.py --list   # show the symbol table

Why this exists
---------------
`strcasecmp` is POSIX. MSVC has `_stricmp`. A file that calls the POSIX name compiles and
links perfectly on Linux and macOS, and fails ONLY on the Windows runner with

    error LNK2019: unresolved external symbol strcasecmp

which is a CI-only failure by construction -- the local build cannot see it. That is exactly
what happened when redirect handling was added to edc_https_client.c: green locally, red on
`windows/amd64` after the push.

The repository already has the fix mechanism (`edc_compat.h` maps these names, see
src/common/win_compat/strings.h) and already uses it in every other file that needs it. What
was missing was anything that NOTICES when a new call site forgets. This is that thing.

It is deliberately a static check rather than a Windows build: a `bazel build` for another
platform is minutes and needs a toolchain, while this is milliseconds and runs in the same
suite as everything else.
"""
import pathlib
import re
import sys

# Symbols MSVC does not provide under these names, mapped by edc_compat.h (or win_compat/).
#
# Deliberately NARROW. `strdup` and `strndup` are spelled the same by MSVC's <string.h> (as
# deprecated POSIX names), so flagging them produced five hits on files that have linked on
# Windows for months -- a checker that cries wolf on working code gets switched off. Only the
# symbols that genuinely fail to link belong here.
POSIX_ONLY = {
    "strcasecmp": "_stricmp",
    "strncasecmp": "_strnicmp",
}

# The shim. Any file using the symbols above must reach it.
#
# The include may be path-qualified ("common/edc_compat.h"), so the pattern allows a path.
SHIM = "edc_compat.h"

# Files that ARE the shim, or that are Windows-only by construction and never compiled by
# MSVC as POSIX. `win_compat/` implements the symbols; edc_compat.h declares the mappings.
EXEMPT_SUBSTRINGS = (
    "win_compat/",
    "edc_compat.h",
)

CALL_RE = re.compile(r"\b(" + "|".join(re.escape(s) for s in POSIX_ONLY) + r")\s*\(")
# `#include "edc_compat.h"`, `#include <edc_compat.h>`, and path-qualified spellings such as
# `#include "common/edc_compat.h"` -- a file that reaches the shim by any of these is fine.
INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"](?:[^">]*/)?' + re.escape(SHIM) + r'[>"]', re.MULTILINE
)


def strip_comments_and_strings(text: str) -> str:
    """Remove comments and string/char literals so a mention in prose is not a call site.

    Without this the checker fires on the comment explaining the very rule it enforces --
    which is what happened the first time it was written.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
        elif c in ('"', "'"):
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check_file(path: pathlib.Path) -> list[str]:
    problems = []
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"{path}: cannot read: {exc}"]

    code = strip_comments_and_strings(raw)
    calls = sorted({m.group(1) for m in CALL_RE.finditer(code)})
    if not calls:
        return []

    if INCLUDE_RE.search(raw):
        return []

    for sym in calls:
        problems.append(
            f"{path}: uses `{sym}` (POSIX; MSVC has `{POSIX_ONLY[sym]}`) "
            f"but does not include \"{SHIM}\" -- this links on Linux and fails on Windows"
        )
    return problems


def main() -> int:
    if "--list" in sys.argv[1:]:
        for sym, msvc in sorted(POSIX_ONLY.items()):
            print(f"  {sym:12} -> {msvc}")
        return 0

    root = pathlib.Path("src")
    if not root.is_dir():
        print("check_portability: run from the repository root", file=sys.stderr)
        return 2

    problems = []
    seen = 0
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".c", ".h", ".cc", ".cpp", ".hpp"):
            continue
        if any(s in path.as_posix() for s in EXEMPT_SUBSTRINGS):
            continue
        seen += 1
        problems.extend(check_file(path))

    if problems:
        for p in problems:
            print(p, file=sys.stderr)
        print(f"\ncheck_portability: {len(problems)} problem(s) across {seen} file(s)", file=sys.stderr)
        return 1

    print(f"check_portability: {seen} file(s), no unshimmed POSIX-only calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
