#!/bin/sh
# Install a released EdgeClaw binary.
#
#   curl -fsSL https://raw.githubusercontent.com/edgeclaw-chat/edgeclaw/master/tools/install.sh | sh
#   sh tools/install.sh --pre                  # newest release, pre-releases included
#   sh tools/install.sh --tag v1.0.0-rc.1      # a specific tag
#   sh tools/install.sh --musl --dir ~/bin     # musl build, into ~/bin
#   sh tools/install.sh --dry-run              # print what it would do, fetch nothing
#
# Why a script rather than a fixed download URL
# ---------------------------------------------
# The release assets carry the tag in their name (`edgeclaw_amd64_linux_v1.0.0-rc.1.tar.gz`),
# which is what makes an archive identifiable a year later -- but it also means no URL
# written down in a README can point at "the newest release". `releases/latest/download/`
# is not an alternative: that URL resolves an asset BY NAME in the newest non-pre-release,
# and this project's tags carry a suffix, so it 404s (see .github/workflows/release.yml).
# This script closes the gap the way every other project with tag-carrying assets does:
# ask the API which tag is current, then build the filename from the answer.
#
# Why it asks /releases and not /releases/latest
# ----------------------------------------------
# /releases/latest deliberately skips pre-releases. While every tag here has a suffix that
# endpoint answers 404, and it also hides the case --pre exists for. Listing releases and
# picking one gives a single code path for both cases, and one place to explain the choice.
#
# Trust boundary
# --------------
# There is nothing to verify the download against: releases publish no checksums, so TLS
# to GitHub is the whole boundary. `curl -f` catches HTTP errors and the tar listing below
# catches a truncated body, and neither is a substitute for a signature. If checksums are
# ever published, verify them here -- an install script that silently skips verification
# once a checksum exists is worse than one that never claimed to.
#
# POSIX sh, no bashisms: this runs on whatever `sh` a container or a fresh macOS has.

set -eu

REPO="edgeclaw-chat/edgeclaw"
API="https://api.github.com/repos/${REPO}/releases"
DL="https://github.com/${REPO}/releases/download"

# Install location, resolved after the arguments are read: --dir must work on a machine
# where HOME is not set (a bare container, `su` without `-`). ~/.local/bin is the XDG-ish
# default that needs no root; macOS and most Linux distributions already have it on PATH, and
# MSYS/Git Bash maps $HOME the same way.
INSTALL_DIR=""

PIN_TAG=""
ALLOW_PRE=0
PREFER_MUSL=0
DRY_RUN=0

die() {
    printf 'install.sh: %s\n' "$1" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Install a released EdgeClaw binary.

Usage: install.sh [options]

Options:
  --tag <tag>     Install this exact release (a pre-release works, e.g. v1.0.0-rc.1).
  --pre           When no --tag is given, allow the newest release to be a pre-release.
                  Without it, only a stable release is chosen -- and since every release
                  so far is a pre-release, that currently fails with an explanation.
  --musl          On Linux, install the musl build instead of the glibc one.
  --dir <path>    Install into <path> instead of ~/.local/bin.
  --dry-run       Resolve and print tag, target and URL, then stop. Fetches no archive.
  -h, --help      This text.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --tag)      [ $# -ge 2 ] || die "--tag needs a value"; PIN_TAG="$2"; shift 2 ;;
        --pre)      ALLOW_PRE=1; shift ;;
        --musl)     PREFER_MUSL=1; shift ;;
        --dir)      [ $# -ge 2 ] || die "--dir needs a value"; INSTALL_DIR="$2"; shift 2 ;;
        --dry-run)  DRY_RUN=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *)          usage >&2; die "unknown argument: $1" ;;
    esac
done

if [ -z "$INSTALL_DIR" ] && [ -n "${HOME:-}" ]; then
    INSTALL_DIR="${HOME}/.local/bin"
fi

# --------------------------------------------------------------------------- fetching

# One fetcher, chosen once. curl's -f turns a 404 into an error rather than a page of HTML
# being written to the archive; wget -q does the same by failing on HTTP errors.
#
# Neither present is not a dead end, so it does not read like one: macOS and every mainstream
# Linux ship curl, but a minimal container may have neither, and the archive can still be
# fetched by anything else. Say how, rather than only saying no.
if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL "$1"; }
    download() { curl -fsSL -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -qO- "$1"; }
    download() { wget -qO "$2" "$1"; }
else
    die "neither curl nor wget is available, so this script cannot download anything
Install one of them (for example: apt-get install -y curl), or download the release archive
directly with whatever the machine does have and unpack it:

  https://github.com/${REPO}/releases/latest

Each archive holds one self-contained binary; unpack it and run it. There is nothing to
configure."
fi

# ------------------------------------------------------------------- resolving the tag

# Emits "<tag> <stable|pre>" per release, newest first.
#
# Parsed with awk rather than jq on purpose: an install script is copied and piped into a
# shell before anyone reads it, so every extra dependency is a reason for it not to run.
# GitHub's API pretty-prints by default -- one field per line, objects indented two spaces
# and closed with "  }," -- which is what this walks. `"draft": true` releases are skipped:
# they are invisible on the Releases page, so an asset fetched from one would be a file
# nobody can point at.
list_releases() {
    fetch "${API}?per_page=20" | awk '
        /"tag_name":/   { tag = $0; sub(/.*"tag_name": *"/, "", tag); sub(/".*/, "", tag) }
        /"prerelease":/ { pre = ($0 ~ /"prerelease": *true/) }
        /"draft":/      { draft = ($0 ~ /"draft": *true/) }
        /^  \},?$/      { if (tag != "" && !draft) print tag, (pre ? "pre" : "stable")
                          tag = ""; pre = 0; draft = 0 }
    '
}

resolve_tag() {
    if [ -n "$PIN_TAG" ]; then
        printf '%s\n' "$PIN_TAG"
        return 0
    fi

    releases=$(list_releases) || die "could not read the release list from ${API}"
    [ -n "$releases" ] || die "no published releases found at ${API}"

    if [ "$ALLOW_PRE" = 1 ]; then
        printf '%s\n' "$releases" | head -n 1 | cut -d' ' -f1
        return 0
    fi

    stable=$(printf '%s\n' "$releases" | awk '$2 == "stable" { print $1; exit }')
    if [ -z "$stable" ]; then
        # Not a generic failure: this is the expected state before the first stable tag,
        # so say what to do instead of just reporting that nothing was found.
        die "every published release is a pre-release, so there is no stable one to pick.
Pass --pre to install the newest release anyway, or --tag <tag> to choose one:
$(printf '%s\n' "$releases" | sed 's/^/  /')"
    fi
    printf '%s\n' "$stable"
}

# ------------------------------------------------------------------ platform selection

# Maps this machine onto one of the matrix targets in .github/workflows/release.yml. The
# names are not derivable from uname output, so the mapping is spelled out -- a wrong guess
# here would 404 at download time, which the error messages do cover, but an explicit table
# is easier to keep in step with the workflow's `target:` entries.
detect_target() {
    os=$(uname -s)
    machine=$(uname -m)

    case "$machine" in
        x86_64|amd64)           arch=amd64 ;;
        aarch64|arm64)          arch=aarch64 ;;
        armv7*|armv6*|armhf|arm) arch=arm ;;
        *) die "unsupported CPU architecture: ${machine}" ;;
    esac

    case "$os" in
        Linux)
            target="edgeclaw_${arch}_linux"
            if [ "$PREFER_MUSL" = 1 ]; then
                target="${target}_musl"
            fi
            ;;
        Darwin)
            # Only an Apple Silicon build is published; there is no amd64 macOS target.
            [ "$arch" = aarch64 ] || die "no macOS build for ${machine} -- only aarch64 is published"
            if [ "$PREFER_MUSL" = 1 ]; then
                die "--musl applies to Linux only"
            fi
            target="edgeclaw_aarch64_macos"
            ;;
        MINGW*|MSYS*|CYGWIN*)
            [ "$arch" = amd64 ] || die "no Windows build for ${machine} -- only amd64 is published"
            if [ "$PREFER_MUSL" = 1 ]; then
                die "--musl applies to Linux only"
            fi
            target="edgeclaw_amd64_windows"
            ;;
        *)
            die "unsupported operating system: ${os}"
            ;;
    esac

    printf '%s\n' "$target"
}

# ------------------------------------------------------------------------------- main

target=$(detect_target)
tag=$(resolve_tag)
asset="${target}_${tag}.tar.gz"
url="${DL}/${tag}/${asset}"

if [ "$DRY_RUN" = 1 ]; then
    printf 'tag       %s\n' "$tag"
    printf 'target    %s\n' "$target"
    printf 'asset     %s\n' "$asset"
    printf 'url       %s\n' "$url"
    if [ -n "$INSTALL_DIR" ]; then
        printf 'installdir %s\n' "$INSTALL_DIR"
    else
        printf 'installdir (none: HOME is unset, pass --dir <path>)\n'
    fi
    exit 0
fi

# Refuse now rather than after the download: with nowhere to put the binary, fetching it is
# only wasted time, and the failure would otherwise be reported against the download.
if [ -z "$INSTALL_DIR" ]; then
    die "HOME is not set, so there is no default install directory -- pass --dir <path>"
fi

# A temp directory outside the install dir, so a failed download leaves nothing behind that
# looks installed. $$ keeps two concurrent runs from deleting each other's work.
tmp=$(mktemp -d "${TMPDIR:-/tmp}/edgeclaw-install.XXXXXX") || die "could not create a temp directory"
trap 'rm -rf "$tmp"' EXIT INT TERM

printf 'Downloading %s (%s)\n' "$asset" "$tag"
download "$url" "${tmp}/${asset}" || die "download failed: ${url}
The tag exists but has no ${target} asset, or the network is unavailable. Check the assets
at https://github.com/${REPO}/releases/tag/${tag}"

# ── what was actually downloaded ────────────────────────────────────────────────────────
# The binary this script installs runs with the user's privileges and holds their
# conversations and API key. Until now nothing here could tell a release archive from
# whatever else arrived over the same TLS connection, and the script said as much: "TLS to
# GitHub is the whole boundary".
#
# So every archive is checked twice, and the ORDER matters:
#
#   1. verify the signature over sha256sums.txt   -- this is what makes the file trustworthy
#   2. compare the archive against the checksum it lists
#
# Comparing before verifying would accept an attacker who changed both the archive and the
# sums file. Verifying first means the checksum we compare against is one we trust.
#
# Both failures are fatal. A release that cannot be verified is not installed, and there is
# deliberately no flag to skip it: an installer that has an "ignore the signature" switch is
# one where that switch gets used.
verify_archive() {
    _sums="${tmp}/sha256sums.txt"
    _sig="${tmp}/sha256sums.txt.sig"
    _sums_url="${DL}/${tag}/sha256sums.txt"
    _sig_url="${DL}/${tag}/sha256sums.txt.sig"

    download "$_sums_url" "$_sums" || die "could not download ${_sums_url}
This release was published without checksums, so it cannot be verified and will not be
installed. Use a release that includes sha256sums.txt, or build from source."
    download "$_sig_url" "$_sig" || die "could not download ${_sig_url}
The checksums are not signed, so they could have been changed together with the archive.
Refusing to install."

    command -v openssl >/dev/null 2>&1 || die "openssl is required to verify the release signature
Install openssl (usually part of the base system) and run this script again. There is no
supported way to skip this check."

    cat > "${tmp}/pubkey.pem" <<'EDGECLAW_UPDATE_PUBKEY'
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEUpTD9xJjm+ZfICQraLvCruln6hbY
DU6ZgAlq/8hcrdOd/fF0L28GFF4rkCzwf3VkPUhu5ozsA7OzLw1mJvsptQ==
-----END PUBLIC KEY-----
EDGECLAW_UPDATE_PUBKEY

    openssl base64 -d -A -in "$_sig" -out "${tmp}/sums.sig" 2>/dev/null \
        || die "the signature file is not valid base64"
    openssl dgst -sha256 -verify "${tmp}/pubkey.pem" -signature "${tmp}/sums.sig" "$_sums" \
        >/dev/null 2>&1 \
        || die "the release signature does not verify
Either this release was not signed with the EdgeClaw release key, or sha256sums.txt was
modified after signing. Neither is safe to install. Do not continue; report it at
https://github.com/${REPO}/issues"

    _want=$(awk -v a="$asset" '$2 == a { print $1 }' "$_sums")
    [ -n "$_want" ] || die "${asset} is not listed in sha256sums.txt
The release is signed but does not cover this platform's archive, so it cannot be verified."

    # macOS ships `shasum`, most Linux and Git-Bash ship `sha256sum`; this script runs on
    # all of them and has no other hashing option.
    if command -v sha256sum >/dev/null 2>&1; then
        _have=$(sha256sum "${tmp}/${asset}" | cut -d' ' -f1)
    else
        _have=$(shasum -a 256 "${tmp}/${asset}" | cut -d' ' -f1)
    fi
    [ "$_have" = "$_want" ] || die "the downloaded archive does not match its checksum
  expected $_want
  got      $_have
The download is corrupt or was replaced in transit. Delete it and run the script again."

    printf 'Verified signature and checksum\n'
}
verify_archive

# Checks the gzip stream end to end before anything is unpacked or installed: a truncated
# download fails here rather than as a corrupt binary later.
tar -tzf "${tmp}/${asset}" >/dev/null 2>&1 || die "the downloaded archive is not a valid tarball"

# The tarball holds one directory named after the target. Look the binary up instead of
# reconstructing the path: that is what the release workflow's own Unpack step does, after a
# reconstructed path silently produced "produced no binary" on a build that had just linked.
mkdir "${tmp}/x"
tar -xzf "${tmp}/${asset}" -C "${tmp}/x" || die "could not extract the archive"

# The candidate order is per-platform, and that is not cosmetic. On MSYS/Git Bash,
# `[ -f .../edgeclaw ]` returns TRUE when only `edgeclaw.exe` exists -- the runtime appends
# the extension when the literal name is missing -- so testing the extension-less name first
# on Windows yields a path whose basename is "edgeclaw", and the binary would be installed
# without its extension. The release workflow hit the same trap in its Unpack step, which is
# why that step reads the archive's contents instead of guessing a name.
case "$target" in
    *_windows) names="edgeclaw.exe edgeclaw" ;;
    *)         names="edgeclaw edgeclaw.exe" ;;
esac

binary=""
for name in $names; do
    for candidate in "${tmp}"/x/*/"$name"; do
        if [ -f "$candidate" ]; then
            binary="$candidate"
            break
        fi
    done
    if [ -n "$binary" ]; then
        break
    fi
done
[ -n "$binary" ] || die "the archive contains no edgeclaw binary"

mkdir -p "$INSTALL_DIR" || die "could not create ${INSTALL_DIR}"
[ -w "$INSTALL_DIR" ] || die "${INSTALL_DIR} is not writable"

dest="${INSTALL_DIR}/${binary##*/}"

# Move into place atomically and in one filesystem: a reader either sees the old binary or
# the new one, never a half-written file. `install` would copy through the same buffer but
# is not in busybox-less minimal images often enough to rely on.
cp "$binary" "${dest}.tmp.$$" || die "could not write into ${INSTALL_DIR}"
chmod 0755 "${dest}.tmp.$$"
mv -f "${dest}.tmp.$$" "$dest" || die "could not move the binary into place"

printf 'Installed %s\n' "$dest"

case ":${PATH}:" in
    *":${INSTALL_DIR}:"*) ;;
    *)
        printf '\n%s is not on your PATH. Add it, for example:\n' "$INSTALL_DIR"
        printf '  echo '\''export PATH="%s:$PATH"'\'' >> ~/.profile\n' "$INSTALL_DIR"
        ;;
esac
