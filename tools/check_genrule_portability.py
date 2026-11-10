#!/usr/bin/env python3
"""Reject genrules that have no Windows form.

    python3 tools/check_genrule_portability.py           # check every BUILD file
    python3 tools/check_genrule_portability.py --list    # show what was inspected

Why this exists
---------------
A genrule's `cmd` is a POSIX shell command. On Windows Bazel runs it through `bash`, which a
normal Windows machine does not have -- so the build depended on setting `BAZEL_SH` to a Git
for Windows bash before anything would compile. That is a real cost for anyone who just wants
to build the project, and it is invisible on Linux, where the whole thing works.

The fix is `cmd_bat`: Bazel uses it on Windows instead of `cmd`, so no shell is involved. A
genrule without one silently reintroduces the bash requirement, and nothing about adding a
rule reminds you. This check does.

What it does NOT demand: that the two commands differ. Most rules here are one tool
invocation, so `cmd_bat` repeats `cmd` verbatim -- which is the point, since the alternative
was a shell wrapper nobody asked for.

`genrule` allows `cmd_bash` as a synonym for `cmd`; either satisfies the POSIX half.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(".")

# Bazel reads BUILD for the main package and BUILD.bazel elsewhere; this tree uses BUILD, but
# accept both so a new package does not have to remember.
BUILD_NAMES = ("BUILD", "BUILD.bazel")

# Attributes that make a genrule Windows-native. `cmd_bat` runs under cmd.exe.
WINDOWS_ATTR = "cmd_bat"

GENRULE_RE = re.compile(r"^genrule\($", re.MULTILINE)


def strip_hash_comments(text: str) -> str:
    """Drop `# ...` comments so a commented-out attribute is not counted."""
    out = []
    for line in text.split("\n"):
        # Only strip outside string literals: a `#` inside a quoted string is content.
        in_str = None
        cut = len(line)
        for i, ch in enumerate(line):
            if in_str:
                if ch == "\\":
                    continue
                if ch == in_str:
                    in_str = None
            elif ch in ("'", '"'):
                in_str = ch
            elif ch == "#":
                cut = i
                break
        out.append(line[:cut])
    return "\n".join(out)


def balanced_block(text: str, start: int) -> str:
    """Return the text of the call starting at |start|, up to its matching close paren."""
    depth = 0
    i = start
    in_str = None
    while i < len(text):
        ch = text[i]
        if in_str:
            if ch == "\\":
                i += 2
                continue
            if ch == in_str:
                in_str = None
        elif ch in ("'", '"'):
            in_str = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return text[start: i + 1]
        i += 1
    return text[start:]


def check_file(path: pathlib.Path) -> list[str]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_hash_comments(raw)

    problems = []
    for m in GENRULE_RE.finditer(text):
        block = balanced_block(text, m.start())
        name_m = re.search(r'name\s*=\s*"([^"]+)"', block)
        name = name_m.group(1) if name_m else "(unnamed)"
        if not re.search(rf"^\s*{WINDOWS_ATTR}\s*=", block, re.MULTILINE):
            problems.append(
                f"{path}: genrule '{name}' has no {WINDOWS_ATTR} -- on Windows Bazel would run "
                f"its cmd through bash, which a stock machine does not have"
            )
    return problems


def main() -> int:
    if "--list" in sys.argv[1:]:
        print(f"checked attribute: {WINDOWS_ATTR}")
        print(f"build files matched: {', '.join(BUILD_NAMES)}")
        return 0

    build_files = []
    for p in sorted(ROOT.rglob("*")):
        if p.name not in BUILD_NAMES:
            continue
        # bazel-* symlinks point back into the output tree and would double-count sources.
        if any(part.startswith("bazel-") for part in p.parts):
            continue
        build_files.append(p)

    problems = []
    for p in build_files:
        problems.extend(check_file(p))

    if problems:
        for msg in problems:
            print(msg, file=sys.stderr)
        print(f"\ncheck_genrule_portability: {len(problems)} problem(s)", file=sys.stderr)
        return 1

    print(f"check_genrule_portability: {len(build_files)} BUILD file(s), every genrule has {WINDOWS_ATTR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
