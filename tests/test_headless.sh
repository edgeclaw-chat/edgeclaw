#!/usr/bin/env bash
# tests/test_headless.sh -- the CLI contract of --print / --mode json.
#
# What this pins is the part that can be checked without a provider: which flags select a
# headless mode, and the exit codes + messages for "no prompt" and "not configured". The
# authenticated path (a real completion landing on stdout) needs real credentials and is
# therefore verified by hand -- see docs/edgeclaw.md.
#
# The point of testing the failure paths at all: a headless mode is used from cron and CI,
# where a silent 0 exit is worse than a crash.

set -u

BIN="${TEST_SRCDIR}/${TEST_WORKSPACE}/src/tui/edgeclaw"
fails=0
checks=0

check() { # check <description> <condition-result>
    checks=$((checks + 1))
    if [ "$2" -eq 0 ]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1"
        fails=$((fails + 1))
    fi
}

# A master key is needed for the app to START at all. Set a dummy one up front so the
# failure cases below actually build and tear down an app -- that teardown is where the
# runner's start/free race lived (a runner freed before its thread had started aborted).
export EDGECLAW_MASTER_KEY=dummy-for-the-crash-path

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "headless CLI: mode selection and the error contract"

# 1. No prompt: usage error, exit 2, and it must SAY what is missing.
err="$("$BIN" -p 2>&1 >/dev/null)"; rc=$?
[ "$rc" -eq 2 ]; check "no prompt -> exit 2 (got $rc)" $?
echo "$err" | grep -q "no prompt"; check "no prompt -> names the problem" $?

# 2. --mode json is recognised (it must fail on the prompt, not on the flag).
err="$("$BIN" --mode json 2>&1 >/dev/null)"; rc=$?
[ "$rc" -eq 2 ]; check "--mode json with no prompt -> exit 2 (got $rc)" $?
echo "$err" | grep -q "no prompt"; check "--mode json -> names the problem" $?

# 3. An unconfigured home: exit 2 and a message that names both realistic causes
#    (no setup yet, or no master key without a terminal). Silently exiting 0 here would
#    make a broken cron job look healthy.
err="$(HOME="$work" "$BIN" -p "hello" 2>&1 >/dev/null)"; rc=$?
[ "$rc" -eq 2 ]; check "unconfigured home -> exit 2 (got $rc)" $?
echo "$err" | grep -qi "setup\|master key"; check "unconfigured home -> says how to fix it" $?

# 4. Nothing may reach stdout on a failure: `> answer.md` must not collect an error text.
out="$(HOME="$work" "$BIN" -p "hello" 2>/dev/null)"
[ -z "$out" ]; check "a failed run writes nothing to stdout" $?

# 5. A COMMAND is not a task: it must answer without a provider, without a model call, and
#    without creating a group. This is the path that used to hang -- a command sends a
#    system message, so waiting for a task to appear waited for ever.
out="$(HOME="$work" "$BIN" -p "/help" 2>/dev/null)"; rc=$?
[ "$rc" -eq 0 ]; check "a command exits 0 without a provider (got $rc)" $?
[ -n "$out" ]; check "and its answer reaches stdout" $?
echo "$out" | grep -q "Commands"; check "and it really is the command reference" $?

# 6. Timeout: exit 3 and nothing on stdout. Forced with a 1-second budget on a task that
#    cannot finish that fast (and with no provider here, it cannot even start the request).
out="$(HOME="$work" "$BIN" -p "hello" --timeout 1 2>/dev/null)"; rc=$?
{ [ "$rc" -eq 2 ] || [ "$rc" -eq 3 ]; }; check "a short timeout exits 2 or 3, never 0 (got $rc)" $?
[ -z "$out" ]; check "and prints no answer" $?

# 7. --list-sessions: works with no provider (it is a command), and prints the id a script
#    can pass to --session.
out="$(HOME="$work" "$BIN" --list-sessions 2>/dev/null)"; rc=$?
[ "$rc" -eq 0 ]; check "--list-sessions exits 0 with no provider (got $rc)" $?
echo "$out" | grep -q "Sessions"; check "--list-sessions prints the session list" $?

# 8. An unknown --session must fail loudly, and `--session <x>` must never be folded into
#    the prompt as text. This home has no provider, so the provider check fires first
#    (exit 2) -- the "no such session" path itself needs a configured provider and is
#    covered by the run documented in docs/edgeclaw.md, not here.
err="$(HOME="$work" "$BIN" --session no-such-session -p "hello" --timeout 1 2>&1 >/dev/null)"; rc=$?
[ "$rc" -ne 0 ]; check "an unknown --session fails, never exits 0 (got $rc)" $?

# The `start` event carries the prompt, and JSON mode emits it before the provider check --
# so this is where "the flags did not leak into the prompt" is observable without a provider.
start="$(HOME="$work" "$BIN" --mode json --session no-such-session --timeout 1 "hello there" 2>/dev/null | head -1)"
echo "$start" | grep -q '"prompt":"hello there"'; check "--session is not folded into the prompt (got: $start)" $?

echo "${checks} checks, ${fails} failed"
exit $((fails == 0 ? 0 : 1))
