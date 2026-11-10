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
python3 - "$RUN_PS1" "$SIGNPUB" <<'PYEOF'
import sys
script, pub = sys.argv[1], sys.argv[2]
text = open(script, encoding="utf-8").read()
pem = open(pub, encoding="utf-8").read().strip()
marker = "@'\n"
start = text.index(marker) + len(marker)
end = text.index("\n'@\n", start)
open(script, "w", encoding="utf-8").write(text[:start] + pem + text[end:])
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
python3 -m http.server --directory "${WORK}/fixtures" 0 >"${WORK}/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$WORK"' EXIT
for _ in $(seq 1 50); do
    PORT=$(sed -n 's/.*port \([0-9]*\).*/\1/p' "${WORK}/server.log" | head -1)
    [ -n "${PORT:-}" ] && break
    sleep 0.1
done
[ -n "${PORT:-}" ] || { echo "could not start the fixture server" >&2; exit 1; }

# The script resolves assets from github.com; point it at the fixture server by overriding the
# download base. sed on a copy keeps the shipped script free of test hooks.
sed -i "s#^\\\$Dl = .*#\$Dl = 'http://127.0.0.1:${PORT}'#" "$RUN_PS1" 2>/dev/null || \
    perl -0pi -e "s{^\\\$Dl = .*}{\$Dl = 'http://127.0.0.1:${PORT}'}m" "$RUN_PS1"
grep -q "127.0.0.1:${PORT}" "$RUN_PS1" || { echo "could not point the script at the fixture server" >&2; exit 1; }

run_ps1() {
    pwsh -NoProfile -NonInteractive -File "$RUN_PS1" "$@" 2>&1
}

# ── --dry-run needs no network for the asset, only the tag is given ───────────────────────
out=$(run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin" -DryRun)
check "$?" "0" "a dry run succeeds"
check "$(printf '%s\n' "$out" | grep -c 'edgeclaw_amd64_windows_v9.9.9.tar.gz')" "1" "the dry run names the asset"
check "$(printf '%s\n' "$out" | grep -c 'edgeclaw_amd64_windows')" "1" "the dry run names the target"
check "$(ls -A "${WORK}/bin" | wc -l | tr -d ' ')" "0" "a dry run installs nothing"

# ── a correctly signed release installs ──────────────────────────────────────────────────
mk_archive v9.9.9
out=$(run_ps1 -Tag v9.9.9 -Dir "${WORK}/bin")
check "$?" "0" "a correctly signed release installs"
check "$(ls "${WORK}/bin")" "edgeclaw.exe" "installed as edgeclaw.exe"
check "$(cat "${WORK}/bin/edgeclaw.exe")" "fake binary for v9.9.9" "the verified binary is the one in place"
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
