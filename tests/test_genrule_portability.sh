#!/usr/bin/env bash
# tests/test_genrule_portability.sh -- the lint that rejects genrules with no Windows form.
#
# A genrule's cmd is a POSIX shell command, and on Windows Bazel runs it through bash -- which
# a stock Windows machine does not have. `cmd_bat` is what removes that requirement. Nothing
# about adding a rule reminds you, so this check does.
#
# The self-tests matter more than the real-tree run: a lint that always passes looks exactly
# like a clean tree.

set -u

ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
CHECKER="${ROOT}/tools/check_genrule_portability.py"
[ -f "$CHECKER" ] || { echo "missing: $CHECKER" >&2; exit 1; }
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

# Run the checker against a fixture tree. $1 label, $2 BUILD body, $3 want-exit.
fixture() {
    label=$1
    body=$2
    want=$3
    d=$(mktemp -d)
    mkdir -p "$d/tools"
    # The checker looks for BUILD files from its cwd, so run it there.
    cp "$CHECKER" "$d/check.py"
    printf '%s' "$body" > "$d/BUILD"
    ( cd "$d" && python3 check.py >/dev/null 2>&1 )
    got=$?
    rm -rf "$d"
    check "$got" "$want" "$label"
}

# ---------------------------------------------------------------- the real tree
#
# NOT run here: runfiles hold only declared data, so the repository's BUILD files are not
# beside this script. The real-tree check is a CI step; what this file proves is that the
# checker can fail.

# ---------------------------------------------------------------- self-tests

fixture "a genrule without cmd_bat is rejected" \
'genrule(
    name = "r",
    outs = ["o"],
    cmd = "tool > $@",
)
' 1

fixture "a genrule WITH cmd_bat is accepted" \
'genrule(
    name = "r",
    outs = ["o"],
    cmd = "tool > $@",
    cmd_bat = "tool > $@",
)
' 0

fixture "cmd_bashe alone is not enough (Windows still needs one)" \
'genrule(
    name = "r",
    outs = ["o"],
    cmd_bashe = "tool > $@",
)
' 1

# A commented-out attribute must not count: the intent is there, the rule is not.
fixture "a commented-out cmd_bat does not count" \
'genrule(
    name = "r",
    outs = ["o"],
    cmd = "tool > $@",
    # cmd_bat = "tool > $@",
)
' 1

# The check reads genrule blocks, so an unrelated rule must not be flagged.
fixture "other rule types are ignored" \
'cc_library(
    name = "l",
    srcs = ["a.c"],
)
' 0

# Several genrules in one file are all inspected, not just the first.
fixture "every genrule in a file is checked" \
'genrule(
    name = "a",
    outs = ["a.out"],
    cmd = "t > $@",
    cmd_bat = "t > $@",
)

genrule(
    name = "b",
    outs = ["b.out"],
    cmd = "t > $@",
)
' 1

# A `#` inside a quoted string is content, not the start of a comment. Stripping there would
# hide the rest of the line -- including a cmd_bat that follows it.
fixture "a hash inside a string does not start a comment" \
'genrule(
    name = "r",
    outs = ["o"],
    cmd = "echo # > $@",
    cmd_bat = "echo # > $@",
)
' 0

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
