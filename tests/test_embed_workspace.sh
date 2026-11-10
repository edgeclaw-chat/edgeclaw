#!/usr/bin/env bash
# tests/test_embed_workspace.sh -- tools/embed_workspace.py's path handling.
#
# The tool turns a list of files into a C table whose `path` field is the file's name with a
# prefix removed. That name is what the runtime looks the file up by, so a name that does not
# match is a bundle that exists but cannot be found -- which is exactly what happened when
# genrules gained `cmd_bat`: Windows hands the tool `docs\edgeclaw.md`, `startswith("docs/")`
# was false, and every entry was embedded as `docs/...` instead of `...`.
#
# This machine can reproduce that spelling: a backslash is a legal filename character on
# POSIX, so a file literally named `docs\edgeclaw.md` gives the tool the same argv Windows
# would. No Windows runner needed to pin the behaviour.
#
# Also covers the root paths and the pre-existing behaviour, so a fix for one spelling cannot
# quietly break another.

set -u

ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
EMBED="${ROOT}/tools/embed_workspace"
[ -f "$EMBED" ] || EMBED="${ROOT}/tools/embed_workspace.py"
[ -f "$EMBED" ] || { echo "missing: tools/embed_workspace(.py)" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 required" >&2; exit 1; }

fails=0
checks=0
check() {
    checks=$((checks + 1))
    if [ "$1" = "$2" ]; then
        printf 'ok    %s\n' "$3"
    else
        printf 'FAIL  %s\n        got:  %s\n        want: %s\n' "$3" "$1" "$2"
        fails=$((fails + 1))
    fi
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

mkdir -p docs workspace/skills/demo
printf 'the manual\n' > docs/edgeclaw.md
printf 'chapter\n' > docs/limitations.md
printf 'a skill\n' > workspace/skills/demo/SKILL.md

# The name of the single embedded entry, as it appears in the generated table.
only_name() {
    python3 "$EMBED" --out "$1" --prefix "$2" --name t --header t.h "$3" >/dev/null 2>&1 || return 1
    # The generated line looks like: {"edgeclaw.md", data, len},
    grep -oE '\{"[^"]+"' "$1" | head -1 | sed 's/^{"//; s/"$//'
}

# ── the spellings a genrule actually produces ───────────────────────────────────────────
check "$(only_name g1.c docs docs/edgeclaw.md)" "edgeclaw.md" \
    "a posix relative path loses its prefix"
check "$(only_name g2.c workspace workspace/skills/demo/SKILL.md)" "skills/demo/SKILL.md" \
    "a nested posix path keeps its inner directories"
check "$(only_name g3.c docs "$WORK/docs/edgeclaw.md")" "edgeclaw.md" \
    "an absolute path still loses its prefix"

# The Windows spelling: cmd_bat hands over native separators.
printf 'native\n' > 'docs\native.md'
check "$(only_name g4.c docs 'docs\native.md')" "native.md" \
    "a backslash-separated path loses its prefix too"

# ── no prefix: the whole path is the name, still normalised ─────────────────────────────
check "$(only_name g5.c '' docs/edgeclaw.md)" "docs/edgeclaw.md" \
    "without a prefix the path is kept as the name"
check "$(only_name g6.c '' 'docs\native.md')" "docs/native.md" \
    "without a prefix a backslash path is still normalised"

# ── a prefix that does not match is left alone ──────────────────────────────────────────
check "$(only_name g7.c other docs/edgeclaw.md)" "docs/edgeclaw.md" \
    "an unmatched prefix leaves the name unchanged"

# ── more than one file: every entry named, sorted ───────────────────────────────────────
python3 "$EMBED" --out multi.c --prefix docs --name t --header t.h docs/edgeclaw.md docs/limitations.md >/dev/null 2>&1
check "$(grep -cE '\{"[^"]+"' multi.c)" "2" "two files produce two entries"
check "$(grep -oE '\{"[^"]+"' multi.c | sed 's/^{"//; s/"$//' | tr '\n' ' ')" "edgeclaw.md limitations.md " \
    "entries are sorted by name"

# ── the content travels with the name ──────────────────────────────────────────────────
if grep -q 'the manual' multi.c; then
    check "present" "present" "the file's bytes are embedded next to its name"
else
    check "missing" "present" "the file's bytes are embedded next to its name"
fi

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
