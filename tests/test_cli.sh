#!/usr/bin/env bash
# tests/test_cli.sh -- the CLI contract: `edgeclaw_cli '<the line you would have typed>'`.
#
# The design claim under test is that there is NOTHING to learn: the argument is the same
# line the TUI accepts, and the only choice is what to do with the output. So the cases here
# are "the same input the TUI would take", plus the output modes, plus the failure shapes a
# cron job depends on.
#
# The authenticated cases (a real completion, a pipeline) need a provider and are verified
# by hand -- see docs/edgeclaw.md. Everything below runs without one.

set -u

BIN="${TEST_SRCDIR}/${TEST_WORKSPACE}/src/cli/edgeclaw_cli"
fails=0
checks=0

check() {
    checks=$((checks + 1))
    if [ "$2" -eq 0 ]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1"
        fails=$((fails + 1))
    fi
}

# A master key lets the app START, which is what makes the failure cases exercise a real
# create/teardown cycle rather than bailing out at the first line.
export EDGECLAW_MASTER_KEY=dummy-for-the-crash-path
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "edgeclaw_cli: one input line, three output modes"

# 1. No line at all: usage, and the usage text has to SHOW the model (an input line), or a
#    caller would have no idea the flags it expects are not the TUI's.
err="$("$BIN" 2>&1 >/dev/null)"; rc=$?
[ "$rc" -eq 2 ]; check "no input -> exit 2 (got $rc)" $?
echo "$err" | grep -q "/task new"; check "the usage shows the input-line model" $?

# 2. A command is not a task: it must answer with no provider, and print its answer.
out="$(HOME="$work" "$BIN" '/help' 2>/dev/null)"; rc=$?
[ "$rc" -eq 0 ]; check "/help exits 0 without a provider (got $rc)" $?
echo "$out" | grep -q "Commands"; check "/help prints the command reference" $?

# 3. A task that cannot succeed (no usable provider here) exits NON-ZERO and puts nothing on
#    stdout -- `> answer.md` must stay empty. There is no separate "provider missing" check:
#    the line runs, fails, and says why, exactly as it would in the TUI.
out="$(HOME="$work" "$BIN" 'hello' 2>/dev/null)"; rc=$?
[ "$rc" -ne 0 ]; check "a failing task exits non-zero (got $rc)" $?
[ -z "$out" ]; check "and writes nothing to stdout" $?

# 4. Unquoted multi-word input is ONE line, joined with single spaces -- the shell's own
#    splitting must not turn a sentence into separate words of the input.
start="$(HOME="$work" "$BIN" --json 'explain' 'this' 'repo' 2>/dev/null | head -1)"
echo "$start" | grep -q '"content":"explain this repo"'; check "unquoted words join into one input line (got: $start)" $?

# 5. `--` ends the flags, so a line that begins with a dash is still an input line.
start="$(HOME="$work" "$BIN" --json -- '--not-a-flag' 2>/dev/null | head -1)"
echo "$start" | grep -q '"content":"--not-a-flag"'; check "-- lets a line start with a dash (got: $start)" $?

# 6. Output modes: --silent writes nothing, --json writes one object per line, and the
#    default writes the answer only. Checked on a command, which needs no provider.
out="$(HOME="$work" "$BIN" --silent '/help' 2>/dev/null)"; rc=$?
[ "$rc" -eq 0 ]; check "--silent exits 0 (got $rc)" $?
[ -z "$out" ]; check "--silent writes nothing" $?

json="$(HOME="$work" "$BIN" --json '/help' 2>/dev/null)"
echo "$json" | tail -1 | grep -q '"event":"done"'; check "--json ends with a done event" $?
invalid=0
while IFS= read -r line; do
    [ -z "$line" ] && continue
    case "$line" in
        \{*\}) ;;
        *) invalid=1 ;;
    esac
done <<EOF
$json
EOF
[ "$invalid" -eq 0 ]; check "--json emits one JSON object per line" $?

# 7. A command's answer also reaches stderr never, and stdout only: the failure text of a
#    failed run must not leak into the answer stream.
errout="$(HOME="$work" "$BIN" 'hello' 2>&1 >/dev/null)"
[ -n "$errout" ]; check "a failure explains itself on stderr" $?

echo "${checks} checks, ${fails} failed"
exit $((fails == 0 ? 0 : 1))
