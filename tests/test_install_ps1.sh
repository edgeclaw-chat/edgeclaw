#!/usr/bin/env bash
# tests/test_install_ps1.sh -- tools/install.ps1, the Windows installer.
#
# Runs under pwsh on whatever OS the test is invoked on. That is deliberate: the signature
# verification, the checksum comparison and the argument handling are platform-independent
# PowerShell, so a Linux runner exercises the same code paths a Windows user would hit -- and
# it does so without waiting for a Windows matrix entry.
#
# Offline like test_install_sh: the download is replaced by a fixture directory, because the
# interesting cases are the failures (missing checksums, unsigned checksums, a tampered sums
# file, a replaced archive) and those must be reproducible without GitHub.
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

# The committed key is the real one, and its private half is not in this repository -- which
# is the point, but it also means the fixtures cannot be signed with it. So the script under
# test is a COPY whose embedded key has been swapped for a throwaway pair generated here, and
# the verification path runs for real rather than being stubbed out.
if ! command -v openssl >/dev/null 2>&1; then
    echo "SKIP: openssl is required to sign the fixtures"
    exit 0
fi
SIGNKEY="${WORK}/test-signing.key"
SIGNPUB="${WORK}/test-signing.pub.pem"
openssl ecparam -name prime256v1 -genkey -noout -out "$SIGNKEY" 2>/dev/null
openssl ec -in "$SIGNKEY" -pubout -out "$SIGNPUB" 2>/dev/null

RUN_PS1="${WORK}/install-under-test.ps1"
cp "$INSTALL_PS1" "$RUN_PS1"
# Swap the embedded key for the throwaway one, as the raw P-256 point in hex -- the form the
# PowerShell script uses (it cannot use ImportFromPem on Windows PowerShell 5.1).
python3 - "$RUN_PS1" "$SIGNPUB" <<'PYEOF'
import subprocess
import sys

script, pub = sys.argv[1], sys.argv[2]
der = subprocess.run(
    ["openssl", "pkey", "-pubin", "-in", pub, "-outform", "DER"],
    capture_output=True, check=True,
).stdout
marker = b"\x03\x42\x00\x04"
idx = der.rfind(marker)
if idx < 0:
    sys.exit("could not extract the test public key")
point = der[idx + 3: idx + 3 + 65]
if len(point) != 65 or point[0] != 0x04:
    sys.exit("extracted test key is not a 65-byte uncompressed point")
hex_point = point.hex()

text = open(script, encoding="utf-8").read()
opening = "@'\n"
start = text.index(opening) + len(opening)
end = text.index("\n'@\n", start)
open(script, "w", encoding="utf-8").write(text[:start] + hex_point + text[end:])
PYEOF

# Sign every archive in the fixture directory. Called after each fixture is created, so the
# happy paths see a valid signature and the negative cases break exactly one thing.
resign_release() {
    ( cd "${WORK}/fixtures" && sha256sum ./*.tar.gz 2>/dev/null \
        | awk '{n=$2; sub(/.*\//,"",n); print $1"  "n}' | sort -k2 > sha256sums.txt ) \
        || ( cd "${WORK}/fixtures" && shasum -a 256 ./*.tar.gz \
        | awk '{n=$2; sub(/.*\//,"",n); print $1"  "n}' | sort -k2 > sha256sums.txt )
    openssl dgst -sha256 -sign "$SIGNKEY" -out "${WORK}/s.bin" "${WORK}/fixtures/sha256sums.txt"
    openssl base64 -A -in "${WORK}/s.bin" -out "${WORK}/fixtures/sha256sums.txt.sig"
    printf '\n' >> "${WORK}/fixtures/sha256sums.txt.sig"
}

# A published archive: one directory with the binary inside.
mk_archive() {
    tag=$1
    dir="${WORK}/payload/${tag}"
    mkdir -p "$dir"
    printf 'fake binary for %s\n' "$tag" > "$dir/edgeclaw.exe"
    chmod +x "$dir/edgeclaw.exe"
    ( cd "${WORK}/payload" && tar -czf "${WORK}/fixtures/edgeclaw_amd64_windows_${tag}.tar.gz" "${tag}" )
    resign_release
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

# ── a correctly signed release installs ──────────────────────────────────────────────────
mk_archive v9.9.9

# Local sanity check on the fixture, before PowerShell is asked to verify it: the signature
# must be valid to openssl. If this passes and the script's verification fails, the problem is
# in the script rather than in the fixture -- which is the only part of this chain this
# machine can settle without pwsh.
openssl base64 -d -A -in "${WORK}/fixtures/sha256sums.txt.sig" -out "${WORK}/verify.sig"
if openssl dgst -sha256 -verify "$SIGNPUB" -signature "${WORK}/verify.sig" "${WORK}/fixtures/sha256sums.txt" >/dev/null 2>&1; then
    check "valid" "valid" "the fixture's signature is valid to openssl"
else
    check "invalid" "valid" "the fixture's signature is valid to openssl"
fi

out=$(run_ps1_expect_ok "install" -Tag v9.9.9 -Dir "${WORK}/bin")
check "$?" "0" "a correctly signed release installs"
check "$(ls "${WORK}/bin" 2>/dev/null)" "edgeclaw.exe" "installed as edgeclaw.exe"
check "$(cat "${WORK}/bin/edgeclaw.exe" 2>/dev/null)" "fake binary for v9.9.9" "the verified binary is the one in place"
check "$(printf '%s\n' "$out" | grep -c 'Verified signature and checksum')" "1" "the run reports what it verified"

# Installing twice must not accumulate files.
run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1
check "$(ls "${WORK}/bin" | wc -l | tr -d ' ')" "1" "a second install leaves one file"

# ── the signature chain: each case breaks exactly one link ───────────────────────────────

# No checksums at all: the state every release before this feature was in.
mv "${WORK}/fixtures/sha256sums.txt" "${WORK}/sums.bak"
if run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed an unverifiable release" "refused" "a release without checksums is refused"
else
    check "refused" "refused" "a release without checksums is refused"
fi
mv "${WORK}/sums.bak" "${WORK}/fixtures/sha256sums.txt"

# Checksums present but unsigned: they could have been changed with the archive.
mv "${WORK}/fixtures/sha256sums.txt.sig" "${WORK}/sig.bak"
if run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed with an unsigned checksum file" "refused" "an unsigned checksum file is refused"
else
    check "refused" "refused" "an unsigned checksum file is refused"
fi
mv "${WORK}/sig.bak" "${WORK}/fixtures/sha256sums.txt.sig"

# Checksums edited after signing: the signature no longer covers what it says it covers.
cp "${WORK}/fixtures/sha256sums.txt" "${WORK}/sums.bak"
printf 'deadbeef  edgeclaw_amd64_windows_v9.9.9.tar.gz\n' > "${WORK}/fixtures/sha256sums.txt"
if run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed with a tampered checksum file" "refused" "a tampered checksum file is refused"
else
    check "refused" "refused" "a tampered checksum file is refused"
fi
cp "${WORK}/sums.bak" "${WORK}/fixtures/sha256sums.txt"

# The archive replaced after signing: signed sums are fine, the bytes do not match. This is
# the case a checksum alone catches and a signature alone does not.
cp "${WORK}/fixtures/edgeclaw_amd64_windows_v9.9.9.tar.gz" "${WORK}/archive.bak"
printf 'x' >> "${WORK}/fixtures/edgeclaw_amd64_windows_v9.9.9.tar.gz"
if run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed a mismatched archive" "refused" "an archive that does not match its checksum is refused"
else
    check "refused" "refused" "an archive that does not match its checksum is refused"
fi
cp "${WORK}/archive.bak" "${WORK}/fixtures/edgeclaw_amd64_windows_v9.9.9.tar.gz"

# None of the failures replaced the working binary.
check "$(cat "${WORK}/bin/edgeclaw.exe")" "fake binary for v9.9.9" "no failed verification replaced the binary"

# ── a valid archive with no binary in it ─────────────────────────────────────────────────
mkdir -p "${WORK}/payload/empty/lonely"
printf 'x\n' > "${WORK}/payload/empty/lonely/README"
( cd "${WORK}/payload/empty" && tar -czf "${WORK}/fixtures/edgeclaw_amd64_windows_v9.9.7.tar.gz" lonely )
resign_release
if run_ps1 -Tag v9.9.7 -Dir "${WORK}/bin" >/dev/null 2>&1; then
    check "installed from an archive with no binary" "refused" "an archive without the binary is refused"
else
    check "refused" "refused" "an archive without the binary is refused"
fi

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
