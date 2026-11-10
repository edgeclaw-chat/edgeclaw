#!/usr/bin/env bash
# tests/test_portability.sh -- the lint that runs tools/check_portability.py.
#
# A wrapper rather than a sh_test pointing at the checker directly, because the checker has to
# be run from the repository root (it walks src/) and its exit status has to become the test's.
# The interesting part of this file is the self-test: a lint nobody has watched fail is a lint
# nobody knows works.

set -u

ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
CHECKER="${ROOT}/tools/check_portability.py"
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

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "${WORK}/src/common"
cp "$CHECKER" "${WORK}/check_portability.py"

# ---------------------------------------------------------------- the real tree
#
# NOT checked here: a sh_test's runfiles hold only the declared data, so `src/` is not there
# and walking it would silently check nothing. The real-tree run is a CI step in
# .github/workflows/{tests,build,pr-check}.yml, where the full checkout exists. What this file
# proves is that the checker itself can fail -- a lint that always passes looks identical to a
# clean tree.

# ---------------------------------------------------------------- self-tests
#
# These run the checker against fixtures, so the suite proves the checker itself can fail.

neg_case() {
    # $1 label, $2 file body, $3 want-exit (0 or 1)
    printf '%s' "$2" > "${WORK}/src/common/fixture.c"
    if ( cd "$WORK" && python3 check_portability.py >/dev/null 2>&1 ); then
        got=0
    else
        got=1
    fi
    check "$got" "$3" "$1"
}

neg_case "a POSIX-only call without the shim is rejected" \
'#include <string.h>
int f(const char *a, const char *b) { return strcasecmp(a, b); }
' 1

neg_case "the same call WITH the shim is accepted" \
'#include <string.h>
#include "edc_compat.h"
int f(const char *a, const char *b) { return strcasecmp(a, b); }
' 0

neg_case "strncasecmp is checked too" \
'#include <string.h>
int f(const char *a, const char *b) { return strncasecmp(a, b, 3); }
' 1

# The checker strips comments and literals first. Without that it fired on the comment
# explaining the rule -- which is how it was first written.
neg_case "a mention in a comment is not a call site" \
'/* we avoid strcasecmp here on purpose */
#include <string.h>
int f(void) { return 0; }
' 0

neg_case "a mention in a string literal is not a call site" \
'#include <string.h>
const char *s = "strcasecmp"; 
int f(void) { return 0; }
' 0

# A file that includes the shim under a path-qualified name still counts.
neg_case "a path-qualified shim include counts" \
'#include "common/edc_compat.h"
int f(const char *a, const char *b) { return strcasecmp(a, b); }
' 0

# strdup is NOT flagged: MSVC declares it (deprecated POSIX name), and flagging it produced
# five false positives on files that have linked on Windows for months.
neg_case "strdup is not flagged (MSVC declares it)" \
'#include <string.h>
char *f(const char *s) { return strdup(s); }
' 0

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
