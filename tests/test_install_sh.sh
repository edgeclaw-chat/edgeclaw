#!/usr/bin/env bash
# tests/test_install_sh.sh -- tools/install.sh, the one script a user pipes into a shell.
#
# Every case here is offline on purpose. The script's job is: work out which tag is current,
# work out which asset this machine needs, download it, and put the binary somewhere sane.
# A network call would make the first two untestable in CI and flaky everywhere else, so the
# `curl` the script resolves is replaced by a shim that serves fixtures from a directory --
# which also lets the failure paths (404, truncated archive, archive without the binary) be
# exercised, and those are the ones that matter: they decide whether a broken download
# leaves a user with nothing, a stale binary, or a half-written one.
#
# `uname` is shimmed the same way, because the platform table is the one place where a wrong
# answer is silent: it does not crash, it asks for an asset that does not exist. There is no
# way to be aarch64 macOS and x86_64 Linux at once on one machine, so the table is checked by
# telling the script what it is.

set -u

INSTALL_SH="${TEST_SRCDIR}/${TEST_WORKSPACE}/tools/install.sh"
[ -f "$INSTALL_SH" ] || { echo "no install.sh at $INSTALL_SH" >&2; exit 1; }

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
SHIM="${WORK}/shim"
WWW="${WORK}/www"
BINDIR="${WORK}/bin"
mkdir -p "$SHIM" "$WWW" "$BINDIR"

# ------------------------------------------------------------------- the script under test
#
# The real script, not a copy: it needs no test hook. The fetch it does is redirected by the
# `curl` shim below, and the only thing it reads from the release besides the archive is
# sha256sums.txt, which the fixtures provide.
RUN_SH="$INSTALL_SH"

# Write sha256sums.txt for every archive currently in $WWW. Called after each fixture archive
# is created, so the happy paths see a matching checksum and each negative case can break
# exactly one thing.
write_sums() {
    if ! command -v sha256sum >/dev/null 2>&1; then
        # macOS/Git-Bash have shasum only. The script under test handles both; so must this.
        ( cd "$WWW" && shasum -a 256 *.tar.gz \
            | awk '{n=$2; sub(/.*\//,"",n); print $1"  "n}' | sort -k2 > sha256sums.txt )
    else
        ( cd "$WWW" && sha256sum *.tar.gz \
            | awk '{n=$2; sub(/.*\//,"",n); print $1"  "n}' | sort -k2 > sha256sums.txt )
    fi
}

# ---------------------------------------------------------------------------- the shims

cat > "${SHIM}/uname" <<'EOF'
#!/bin/sh
case "$1" in
    -s) printf '%s\n' "${FAKE_OS}" ;;
    -m) printf '%s\n' "${FAKE_ARCH}" ;;
    *)  printf '%s\n' "${FAKE_OS}" ;;
esac
EOF

# Serves ${WORK}/www by asset name. Only the two shapes install.sh uses: `-o FILE URL`
# and a bare URL. Exit 22 is curl's "HTTP error", which is what a missing asset produces.
cat > "${SHIM}/curl" <<'EOF'
#!/bin/sh
out=""
url=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        -*) shift ;;
        *)  url="$1"; shift ;;
    esac
done
name=$(printf '%s' "$url" | sed 's|.*/||')
src="${FAKE_WWW}/${name}"
# The API URL is the only one carrying a query string; a release download also contains
# "/releases/" in its path, so matching that prefix alone would serve the JSON as a tarball.
case "$url" in
    *"releases?per_page="*) src="${FAKE_WWW}/releases.json" ;;
esac
[ -f "$src" ] || exit 22
if [ -n "$out" ]; then cp "$src" "$out"; else cat "$src"; fi
EOF

chmod +x "${SHIM}/uname" "${SHIM}/curl"

# A release list shaped like GitHub's: pretty-printed, one field per line, two-space
# indentation. The parser in install.sh walks exactly this shape.
write_releases() {
    cat > "${WWW}/releases.json" <<EOF
[
  {
    "tag_name": "$1",
    "draft": false,
    "prerelease": $2
  }
]
EOF
}

run_install() {
    # $1..: extra args
    PATH="${SHIM}:${PATH}" FAKE_WWW="$WWW" sh "$RUN_SH" "$@" 2>&1
}

# -------------------------------------------------------------------- platform table

# --tag + --dry-run: no API call, no download, only the mapping.
expect_target() {
    want=$1; os=$2; arch=$3; label=$4; shift 4
    out=$(FAKE_OS="$os" FAKE_ARCH="$arch" run_install --dry-run --tag v9.9.9 "$@")
    got=$(printf '%s\n' "$out" | awk '/^target /{print $2}')
    check "$got" "$want" "${label}"
}

expect_target edgeclaw_amd64_linux       Linux           x86_64  "linux/amd64 -> glibc build"
expect_target edgeclaw_aarch64_linux     Linux           aarch64 "linux/aarch64 -> glibc build"
expect_target edgeclaw_arm_linux         Linux           armv7l  "linux/armv7 -> glibc build"
expect_target edgeclaw_amd64_linux_musl  Linux           x86_64  "linux/amd64 + --musl" --musl
expect_target edgeclaw_arm_linux_musl    Linux           armv7l  "linux/armv7 + --musl" --musl
expect_target edgeclaw_aarch64_macos     Darwin          arm64   "macos/arm64"
expect_target edgeclaw_amd64_windows     MINGW64_NT-10.0 x86_64  "windows/amd64"

# Refusals: a platform with no published build must say so rather than request a 404.
expect_fail() {
    label=$1; os=$2; arch=$3; shift 3
    if FAKE_OS="$os" FAKE_ARCH="$arch" run_install --dry-run --tag v9.9.9 "$@" >/dev/null 2>&1; then
        check "1" "0" "${label} is refused"
    else
        check "1" "1" "${label} is refused"
    fi
}

expect_fail "macos/x86_64 (no build)"  Darwin x86_64
expect_fail "macos + --musl"           Darwin arm64 --musl
expect_fail "unsupported arch"         Linux  riscv64
expect_fail "unsupported OS"           FreeBSD x86_64

# --------------------------------------------------------------- resolving which tag

write_releases "v2.0.0" false
out=$(FAKE_OS=Linux FAKE_ARCH=x86_64 run_install --dry-run)
check "$(printf '%s\n' "$out" | awk '/^tag /{print $2}')" "v2.0.0" "a stable release is picked"

write_releases "v2.0.1-rc.1" true
out=$(FAKE_OS=Linux FAKE_ARCH=x86_64 run_install --dry-run --pre)
check "$(printf '%s\n' "$out" | awk '/^tag /{print $2}')" "v2.0.1-rc.1" "--pre allows a pre-release"

# The state this project is actually in: everything published is a pre-release. Without
# --pre that must fail with advice, not install whatever it found.
if FAKE_OS=Linux FAKE_ARCH=x86_64 run_install --dry-run >/dev/null 2>&1; then
    check "installed a pre-release without --pre" "refused" "a pre-release is not picked implicitly"
else
    check "refused" "refused" "a pre-release is not picked implicitly"
fi

# A draft is invisible on the Releases page, so it must never be selected.
cat > "${WWW}/releases.json" <<'EOF'
[
  {
    "tag_name": "v3.0.0",
    "draft": true,
    "prerelease": false
  },
  {
    "tag_name": "v2.5.0",
    "draft": false,
    "prerelease": false
  }
]
EOF
out=$(FAKE_OS=Linux FAKE_ARCH=x86_64 run_install --dry-run)
check "$(printf '%s\n' "$out" | awk '/^tag /{print $2}')" "v2.5.0" "a draft release is skipped"

# ----------------------------------------------------- environments without a HOME
#
# This is the shape of a build sandbox: Bazel runs actions with a minimal environment, and
# HOME is not in it. Resolution and --dry-run have to work there -- only an actual install
# needs a directory -- and this section exists because getting it wrong turned CI red in a
# way that a Windows dev machine cannot show, since HOME is always set there.

if env -u HOME FAKE_OS=Linux FAKE_ARCH=x86_64 PATH="${SHIM}:${PATH}" FAKE_WWW="$WWW" \
        sh "$INSTALL_SH" --dry-run --tag v9.9.9 >/dev/null 2>&1; then
    check "works" "works" "--dry-run works with HOME unset"
else
    check "failed" "works" "--dry-run works with HOME unset"
fi

out=$(env -u HOME FAKE_OS=Linux FAKE_ARCH=x86_64 PATH="${SHIM}:${PATH}" FAKE_WWW="$WWW" \
      sh "$INSTALL_SH" --dry-run --tag v9.9.9 2>&1)
check "$(printf '%s\n' "$out" | awk '/^target /{print $2}')" "edgeclaw_amd64_linux" \
      "the platform is still resolved with HOME unset"
case "$out" in
    *"(none: HOME is unset"*) check "says so" "says so" "--dry-run says the directory is unresolved" ;;
    *) check "silent" "says so" "--dry-run says the directory is unresolved" ;;
esac

# Installing without HOME and without --dir must refuse with advice, not with an empty path.
out=$(env -u HOME FAKE_OS=Linux FAKE_ARCH=x86_64 PATH="${SHIM}:${PATH}" FAKE_WWW="$WWW" \
      sh "$INSTALL_SH" --tag v9.9.9 2>&1)
check "$?" "1" "installing with HOME unset and no --dir is refused"
case "$out" in
    *"--dir"*) check "advises --dir" "advises --dir" "the refusal mentions --dir" ;;
    *) check "$out" "advises --dir" "the refusal mentions --dir" ;;
esac

# ...and the same environment installs fine once --dir is given.
out=$(env -u HOME FAKE_OS=Linux FAKE_ARCH=armv7l PATH="${SHIM}:${PATH}" FAKE_WWW="$WWW" \
      sh "$INSTALL_SH" --tag v9.9.9 --dir "${WORK}/armbin" 2>&1)
check "$?" "1" "an absent arm asset is still reported when HOME is unset"

# ------------------------------------------------------------ install, end to end

# A published archive, shaped like pkg_tar's output: <target>/<binary> plus licences.
PAYLOAD="${WORK}/payload/edgeclaw_amd64_windows/licenses/sds"
mkdir -p "$PAYLOAD"
printf '#!/bin/sh\necho "edgeclaw v9.9.9 (fake)"\n' > "${WORK}/payload/edgeclaw_amd64_windows/edgeclaw.exe"
printf 'MIT\n' > "${WORK}/payload/edgeclaw_amd64_windows/LICENSE"
printf 'MIT\n' > "$PAYLOAD/LICENSE"
( cd "${WORK}/payload" && tar -czf "${WWW}/edgeclaw_amd64_windows_v9.9.9.tar.gz" edgeclaw_amd64_windows )
write_sums

out=$(FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag v9.9.9 --dir "$BINDIR")
check "$?" "0" "install succeeds"
# The extension matters: on Windows the binary is edgeclaw.exe and a name without it would
# still run from a shell but would not be found by anything that spells the name out.
check "$(ls "$BINDIR")" "edgeclaw.exe" "installed as edgeclaw.exe"
check "$(sh "${BINDIR}/edgeclaw.exe" 2>/dev/null)" "edgeclaw v9.9.9 (fake)" "the installed binary runs"
if [ -x "${BINDIR}/edgeclaw.exe" ]; then
    check "executable" "executable" "the installed binary is executable"
else
    check "not executable" "executable" "the installed binary is executable"
fi

# Installing twice must not accumulate files (a stale .tmp.$$ left behind would).
FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag v9.9.9 --dir "$BINDIR" >/dev/null
check "$(ls "$BINDIR" | wc -l | tr -d ' ')" "1" "a second install leaves one file"

# ------------------------------------------------------------- what a bad download does

# Truncated: right name, broken gzip. Its checksum matches, so the failure is specifically the
# tarball check rather than a checksum mismatch -- the two are different guards and both need
# exercising.
printf 'not a tarball\n' > "${WWW}/edgeclaw_amd64_windows_v9.9.8.tar.gz"
write_sums
if FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag v9.9.8 --dir "$BINDIR" >/dev/null 2>&1; then
    check "installed a corrupt archive" "refused" "a corrupt archive is refused"
else
    check "refused" "refused" "a corrupt archive is refused"
fi
check "$(sh "${BINDIR}/edgeclaw.exe" 2>/dev/null)" "edgeclaw v9.9.9 (fake)" \
      "a failed install leaves the previous binary intact"

# A valid archive that simply has no binary in it.
mkdir -p "${WORK}/payload2/lonely"
printf 'x\n' > "${WORK}/payload2/lonely/README"
( cd "${WORK}/payload2" && tar -czf "${WWW}/edgeclaw_amd64_windows_v9.9.7.tar.gz" lonely )
write_sums
if FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag v9.9.7 --dir "$BINDIR" >/dev/null 2>&1; then
    check "installed from an archive with no binary" "refused" "an archive without the binary is refused"
else
    check "refused" "refused" "an archive without the binary is refused"
fi

# A tag whose asset does not exist at all (the 404 curl reports as exit 22).
if FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag v0.0.1 --dir "$BINDIR" >/dev/null 2>&1; then
    check "installed a missing asset" "refused" "a tag with no asset is refused"
else
    check "refused" "refused" "a tag with no asset is refused"
fi

# --------------------------------------------------------------- the checksum itself
#
# Each case breaks exactly one link and asserts the install is refused, because the failure
# mode that matters is not "the download was rejected" but "an archive nobody checked got
# installed".

# A valid archive with a matching checksum, to compare the negative cases against.
GOODTAG=v9.9.6
mkdir -p "${WORK}/payload3/edgeclaw_amd64_windows"
printf '#!/bin/sh\necho "edgeclaw v9.9.6 (fake)"\n' > "${WORK}/payload3/edgeclaw_amd64_windows/edgeclaw.exe"
( cd "${WORK}/payload3" && tar -czf "${WWW}/edgeclaw_amd64_windows_${GOODTAG}.tar.gz" edgeclaw_amd64_windows )
write_sums

out=$(FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag "$GOODTAG" --dir "$BINDIR")
check "$?" "0" "a release with a matching checksum installs"
check "$(sh "${BINDIR}/edgeclaw.exe" 2>/dev/null)" "edgeclaw v9.9.6 (fake)" \
      "and the checked binary is the one that runs"

# (a) no checksums at all: nothing can be checked, so nothing is installed. This is the case
# where the comparison would otherwise silently do nothing.
mv "${WWW}/sha256sums.txt" "${WORK}/sums.bak"
if FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag "$GOODTAG" --dir "$BINDIR" >/dev/null 2>&1; then
    check "installed a release with no checksums" "refused" "a release without checksums is refused"
else
    check "refused" "refused" "a release without checksums is refused"
fi
mv "${WORK}/sums.bak" "${WWW}/sha256sums.txt"

# (b) the checksum file names a different hash: the archive does not match what the release
# says it should be. Covers both a corrupted download and a swapped archive.
cp "${WWW}/sha256sums.txt" "${WORK}/sums.bak"
printf 'deadbeef  edgeclaw_amd64_windows_%s.tar.gz\n' "$GOODTAG" > "${WWW}/sha256sums.txt"
if FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag "$GOODTAG" --dir "$BINDIR" >/dev/null 2>&1; then
    check "installed with a mismatched checksum file" "refused" "a mismatched checksum is refused"
else
    check "refused" "refused" "a mismatched checksum is refused"
fi
cp "${WORK}/sums.bak" "${WWW}/sha256sums.txt"

# (c) the checksum file does not mention this platform's archive at all: there is nothing to
# compare against, which is different from a mismatch and must also refuse.
cp "${WWW}/sha256sums.txt" "${WORK}/sums.bak"
printf 'deadbeef  edgeclaw_something_else_%s.tar.gz\n' "$GOODTAG" > "${WWW}/sha256sums.txt"
if FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag "$GOODTAG" --dir "$BINDIR" >/dev/null 2>&1; then
    check "installed with an unlisted archive" "refused" "an archive missing from the checksum file is refused"
else
    check "refused" "refused" "an archive missing from the checksum file is refused"
fi
cp "${WORK}/sums.bak" "${WWW}/sha256sums.txt"

# (d) the archive replaced after the checksums were written: the same mismatch as (b) from
# the other direction, and worth its own case because it is what an attacker on the transport
# would actually cause.
cp "${WWW}/edgeclaw_amd64_windows_${GOODTAG}.tar.gz" "${WORK}/archive.bak"
printf 'x' >> "${WWW}/edgeclaw_amd64_windows_${GOODTAG}.tar.gz"
if FAKE_OS=MINGW64_NT-10.0 FAKE_ARCH=x86_64 run_install --tag "$GOODTAG" --dir "$BINDIR" >/dev/null 2>&1; then
    check "installed a mismatched archive" "refused" "an archive that does not match its checksum is refused"
else
    check "refused" "refused" "an archive that does not match its checksum is refused"
fi
cp "${WORK}/archive.bak" "${WWW}/edgeclaw_amd64_windows_${GOODTAG}.tar.gz"

# After all of that, the good binary from before is still what is installed.
check "$(sh "${BINDIR}/edgeclaw.exe" 2>/dev/null)" "edgeclaw v9.9.6 (fake)" \
      "no failed check replaced the working binary"

printf '\n%d checks, %d failed\n' "$checks" "$fails"
[ "$fails" -eq 0 ]
