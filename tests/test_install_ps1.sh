#!/usr/bin/env bash
# tests/test_install_ps1.sh -- tools/install.ps1, the Windows installer.
#
# Runs under pwsh on whatever OS the test is invoked on. That is deliberate: the checksum
# comparison and the argument handling are platform-independent PowerShell, so a Linux runner
# exercises the same code paths a Windows user would hit -- and it does so without waiting for
# a Windows matrix entry.
#
# Offline like test_install_sh: the download is replaced by a fixture directory, because the
# interesting cases are the failures (missing checksums, a mismatched sums file, an archive
# that is not listed, an archive replaced after the sums were written) and those must be
# reproducible without GitHub.
#
# When pwsh is not installed the test SKIPS rather than passes: a silent pass would look like
# coverage that does not exist.

set -u

ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
INSTALL_PS1="${ROOT}/tools/install.ps1"
[ -f "$INSTALL_PS1" ] || { echo "missing: $INSTALL_PS1" >&2; exit 1; }

if ! command -v pwsh >/dev/null 2>&1; then
    echo "SKIP: pwsh is not installed, so install.ps1 cannot be exercised here"
    echo "      (CI installs it; a local run without it proves nothing either way)"
    exit 0
fi

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
mkdir -p "${WORK}/fixtures" "${WORK}/bin"

# The real installer, not a copy: it needs no test hook. What it fetches is redirected to the
# fixture server below, and the only release file it reads besides the archive is
# sha256sums.txt, which the fixtures provide.
RUN_PS1="$INSTALL_PS1"

# Write sha256sums.txt for the archive in a tag's directory. The published layout is
# <download-base>/<tag>/<asset> and <download-base>/<tag>/sha256sums.txt, so the fixture mirrors
# it -- a flat directory answers 404 for the real path shape, which is what an early CI round
# showed.
write_sums() {
    tag=$1
    d="${WORK}/fixtures/${tag}"
    ( cd "$d" && sha256sum ./*.tar.gz 2>/dev/null \
        | awk '{n=$2; sub(/.*\//,"",n); print $1"  "n}' | sort -k2 > sha256sums.txt ) \
        || ( cd "$d" && shasum -a 256 ./*.tar.gz \
        | awk '{n=$2; sub(/.*\//,"",n); print $1"  "n}' | sort -k2 > sha256sums.txt )
}

# A published archive: one directory with the binary inside, served under its tag.
mk_archive() {
    tag=$1
    mkdir -p "${WORK}/payload/${tag}" "${WORK}/fixtures/${tag}"
    printf 'fake binary for %s\n' "$tag" > "${WORK}/payload/${tag}/edgeclaw.exe"
    chmod +x "${WORK}/payload/${tag}/edgeclaw.exe"
    ( cd "${WORK}/payload" && tar -czf "${WORK}/fixtures/${tag}/edgeclaw_amd64_windows_${tag}.tar.gz" "${tag}" )
    write_sums "$tag"
}

# Run the script against the fixtures. A local HTTP server stands in for GitHub: the script
# uses Invoke-WebRequest, which needs a real URL, so serving the files is simpler and more
# faithful than mocking the cmdlet.
#
# The port is chosen BEFORE the server starts and then waited on. Parsing it out of the
# server's own output had two failure modes -- Python buffers the banner, and under a build
# sandbox the redirect may not flush at all -- which showed up as "could not start the fixture
# server" in CI while passing locally.
PORT=$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)
[ -n "${PORT:-}" ] || { echo "could not pick a port for the fixture server" >&2; exit 1; }

python3 -m http.server --bind 127.0.0.1 --directory "${WORK}/fixtures" "$PORT" \
    >"${WORK}/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT

# Wait until it answers, rather than sleeping a guessed amount.
ready=0
for _ in $(seq 1 100); do
    if python3 - "${PORT}" <<'PY' 2>/dev/null
import socket, sys
s = socket.socket()
s.settimeout(0.2)
try:
    s.connect(("127.0.0.1", int(sys.argv[1])))
    sys.exit(0)
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
    then
        ready=1
        break
    fi
    sleep 0.1
done
if [ "$ready" != 1 ]; then
    echo "the fixture server did not come up on port ${PORT}" >&2
    sed 's/^/  server: /' "${WORK}/server.log" >&2 || true
    exit 1
fi

# The script resolves assets from github.com; point it at the fixture server by overriding the
# download base. sed on a copy keeps the shipped script free of test hooks.
sed -i "s#^\\\$Dl = .*#\$Dl = 'http://127.0.0.1:${PORT}'#" "$RUN_PS1" 2>/dev/null || \
    perl -0pi -e "s{^\\\$Dl = .*}{\$Dl = 'http://127.0.0.1:${PORT}'}m" "$RUN_PS1"
grep -q "127.0.0.1:${PORT}" "$RUN_PS1" || { echo "could not point the script at the fixture server" >&2; exit 1; }

run_ps1() {
    pwsh -NoProfile -NonInteractive -File "$RUN_PS1" "$@" 2>&1
}

# Run and fail loudly with the script's own output. Without this a failing check reported only
# "got 1, want 0" in CI, where there is no way to reproduce by hand -- which is exactly how the
# first two rounds of this test went. The output is what turns a red job into a diagnosis.
run_ps1_expect_ok() {
    local label=$1
    shift
    local out status
    out=$(run_ps1 "$@")
    status=$?
    if [ "$status" != 0 ]; then
        printf '\n--- %s: the installer failed (exit %s); its output follows ---\n%s\n--- end ---\n\n' \
            "$label" "$status" "$out" >&2
        return "$status"
    fi
    printf '%s' "$out"
    return 0
}

# The installer reads the target architecture from PROCESSOR_ARCHITECTURE, which on Windows is
# "AMD64" and on the Linux runner is unset. Setting it is not a hack around a check -- it is
# what the test is doing: presenting the environment a Windows x64 machine would present, the
# same way the sh installer's test fakes `uname` output to reach each platform's branch.
export PROCESSOR_ARCHITECTURE=AMD64

# ── --dry-run needs no network for the asset, only the tag is given ───────────────────────
# Fields are read out of their own lines rather than counted: the asset name also appears in
# the URL line, so a plain `grep -c` reports 2 and says nothing about which line carried it.
out=$(run_ps1_expect_ok "dry run" -Tag v9.9.9 -Dir "${WORK}/bin" -DryRun)
check "$?" "0" "a dry run succeeds"
check "$(printf '%s\n' "$out" | awk '/^asset /{print $2}')" "edgeclaw_amd64_windows_v9.9.9.tar.gz" \
    "the dry run names the asset"
check "$(printf '%s\n' "$out" | awk '/^target /{print $2}')" "edgeclaw_amd64_windows" \
    "the dry run names the target"
check "$(printf '%s\n' "$out" | awk '/^tag /{print $2}')" "v9.9.9" "the dry run names the tag"
check "$(ls -A "${WORK}/bin" | wc -l | tr -d ' ')" "0" "a dry run installs nothing"

# ── a release with a matching checksum installs ─────────────────────────────────────────
mk_archive v9.9.9

out=$(run_ps1_expect_ok "install" -Tag v9.9.9 -Dir "${WORK}/bin")
check "$?" "0" "a release with a matching checksum installs"
check "$(ls "${WORK}/bin" 2>/dev/null)" "edgeclaw.exe" "installed as edgeclaw.exe"
check "$(cat "${WORK}/bin/edgeclaw.exe" 2>/dev/null)" "fake binary for v9.9.9" "the checked binary is the one in place"
check "$(printf '%s\n' "$out" | grep -c 'Verified checksum')" "1" "the run reports what it checked"

# Installing twice must not accumulate files.
run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1
check "$(ls "${WORK}/bin" | wc -l | tr -d ' ')" "1" "a second install leaves one file"

# ── the checksum: each case breaks exactly one link ─────────────────────────────────────

# No checksums at all: nothing can be checked, so nothing is installed. This is the case where
# the comparison would otherwise silently do nothing.
mv "${WORK}/fixtures/v9.9.9/sha256sums.txt" "${WORK}/sums.bak"
if run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed a release with no checksums" "refused" "a release without checksums is refused"
else
    check "refused" "refused" "a release without checksums is refused"
fi
mv "${WORK}/sums.bak" "${WORK}/fixtures/v9.9.9/sha256sums.txt"

# The checksum file names a different hash: covers a corrupted download and a swapped archive.
cp "${WORK}/fixtures/v9.9.9/sha256sums.txt" "${WORK}/sums.bak"
printf 'deadbeef  edgeclaw_amd64_windows_v9.9.9.tar.gz\n' > "${WORK}/fixtures/v9.9.9/sha256sums.txt"
if run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed with a mismatched checksum file" "refused" "a mismatched checksum is refused"
else
    check "refused" "refused" "a mismatched checksum is refused"
fi
cp "${WORK}/sums.bak" "${WORK}/fixtures/v9.9.9/sha256sums.txt"

# The checksum file does not mention this platform's archive: nothing to compare against,
# which is different from a mismatch and must also refuse.
cp "${WORK}/fixtures/v9.9.9/sha256sums.txt" "${WORK}/sums.bak"
printf 'deadbeef  edgeclaw_something_else_v9.9.9.tar.gz\n' > "${WORK}/fixtures/v9.9.9/sha256sums.txt"
if run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed with an unlisted archive" "refused" "an archive missing from the checksum file is refused"
else
    check "refused" "refused" "an archive missing from the checksum file is refused"
fi
cp "${WORK}/sums.bak" "${WORK}/fixtures/v9.9.9/sha256sums.txt"

# The archive replaced after the checksums were written: the same mismatch from the other
# direction, and what an attacker on the transport would actually cause.
cp "${WORK}/fixtures/v9.9.9/edgeclaw_amd64_windows_v9.9.9.tar.gz" "${WORK}/archive.bak"
printf 'x' >> "${WORK}/fixtures/v9.9.9/edgeclaw_amd64_windows_v9.9.9.tar.gz"
if run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed a mismatched archive" "refused" "an archive that does not match its checksum is refused"
else
    check "refused" "refused" "an archive that does not match its checksum is refused"
fi
cp "${WORK}/archive.bak" "${WORK}/fixtures/v9.9.9/edgeclaw_amd64_windows_v9.9.9.tar.gz"

# None of the failures replaced the working binary.
check "$(cat "${WORK}/bin/edgeclaw.exe")" "fake binary for v9.9.9" "no failed check replaced the binary"

# ── a valid archive with no binary in it ─────────────────────────────────────────────────
mkdir -p "${WORK}/payload/empty/lonely" "${WORK}/fixtures/v9.9.7"
printf 'x\n' > "${WORK}/payload/empty/lonely/README"
( cd "${WORK}/payload/empty" && tar -czf "${WORK}/fixtures/v9.9.7/edgeclaw_amd64_windows_v9.9.7.tar.gz" lonely )
write_sums v9.9.7
if run_ps1 -Tag v9.9.7 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed from an archive with no binary" "refused" "an archive without the binary is refused"
else
    check "refused" "refused" "an archive without the binary is refused"
fi

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
