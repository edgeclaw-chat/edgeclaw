#!/usr/bin/env bash
# Report whether a Linux binary needs shared libraries at run time.
#
#   tools/check_static.sh bazel-bin/src/tui/edgeclaw
#   tools/check_static.sh bazel-bin/edgeclaw_*.tar.gz     # unpacks and checks
#
# Exits 0 when every binary given is self-contained, 1 otherwise, so it can be used
# as a release gate (`if ! tools/check_static.sh ...; then exit 1; fi`).
#
# Why this does not just use `ldd`:
#   - ldd WORKS BY RUNNING the binary's dynamic loader with LD_TRACE_LOADED_OBJECTS.
#     On a cross-compiled binary it invokes the HOST loader, which produces
#     nonsense: `ldd` on an aarch64 musl binary here prints
#     "error while loading shared libraries: /lib/aarch64-linux-gnu/libc.so:
#     invalid ELF header". It also means ldd executes foreign code -- never run it
#     on something you do not trust.
#   - ldd's wording is inconsistent ("not a dynamic executable" vs "statically
#     linked"), and neither string is a stable interface.
#   readelf/objdump only parse bytes, so they are safe and correct for cross builds.
#
# The decisive field is DT_NEEDED, NOT "does a dynamic section exist":
# a static-PIE binary (what musl and modern glibc -static-pie produce) DOES carry a
# .dynamic section with 20-24 entries -- it needs them for self-relocation -- yet
# has zero DT_NEEDED and no PT_INTERP. Checking "readelf -d printed something"
# therefore misclassifies every static-PIE binary as dynamic.
#
# For PE (Windows) use `objdump -p | awk '/DLL Name/'`; macOS uses otool. This
# script only handles ELF because that is where the subtlety lives.

set -euo pipefail

fail=0
tmpdir=""
cleanup() {
    if [[ -n "${tmpdir}" ]]; then
        rm -rf "${tmpdir}"
    fi
    # Must end on success: this runs as an EXIT trap, so returning nonzero here
    # would override the script's real exit status with 1.
    return 0
}
trap cleanup EXIT

# Prefer the readelf that matches the compiler used, if CROSS_COMPILE is set.
READELF="${CROSS_COMPILE:+${CROSS_COMPILE}readelf}"
if [[ -z "${READELF}" ]] || ! command -v "${READELF}" >/dev/null 2>&1; then
    READELF=readelf
fi
if ! command -v "${READELF}" >/dev/null 2>&1; then
    echo "error: readelf not found (install binutils)" >&2
    exit 2
fi

check_one() {
    local bin="$1"

    if [[ ! -f "${bin}" ]]; then
        echo "error: ${bin}: not a file" >&2
        return 1
    fi

    # Match readelf's own magic so a wrong file type gets a clear message rather
    # than a screen of parse errors.
    if [[ "$(head -c 4 "${bin}" | od -An -tx1 | tr -d ' \n')" != "7f454c46" ]]; then
        echo "SKIP  $(basename "${bin}")  (not ELF -- use objdump -p for PE, otool for Mach-O)"
        return 0
    fi

    local needed interp kind
    needed="$("${READELF}" -d "${bin}" 2>/dev/null | awk '/\(NEEDED\)/{print $NF}' | tr -d '[]' || true)"
    # The path lives on the "[Requesting program interpreter: ...]" line, not in
    # the numeric INTERP segment header.
    interp="$("${READELF}" -l "${bin}" 2>/dev/null \
        | sed -n 's/.*\[Requesting program interpreter: \(.*\)\]/\1/p' || true)"

    # Describe what kind of link this is, for the reader who will hit this output
    # in a log six months from now.
    if [[ -z "${needed}" && -z "${interp}" ]]; then
        if "${READELF}" -d "${bin}" 2>/dev/null | grep -q 'Dynamic section'; then
            kind="static-PIE (has a .dynamic section for self-relocation, but no NEEDED, no INTERP)"
        else
            kind="statically linked"
        fi
    else
        kind="DYNAMIC"
    fi

    if [[ -z "${needed}" && -z "${interp}" ]]; then
        printf 'OK    %-28s %s\n' "$(basename "${bin}")" "${kind}"
        return 0
    fi

    fail=1
    printf 'FAIL  %-28s %s\n' "$(basename "${bin}")" "${kind}"
    if [[ -n "${interp}" ]]; then
        echo "        interpreter: ${interp}"
    fi
    if [[ -n "${needed}" ]]; then
        echo "        needs at run time:"
        while IFS= read -r lib; do
            [[ -n "${lib}" ]] && echo "          - ${lib}"
        done <<< "${needed}"
    fi
    return 0
}

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <binary> [more binaries or .tar.gz/...]..." >&2
    exit 2
fi

for arg in "$@"; do
    case "${arg}" in
        *.tar.gz|*.tgz)
            # The shipped artifact is what matters, not the bazel-out intermediate:
            # pkg_tar could in principle drop or add files.
            tmpdir="$(mktemp -d)"
            tar xzf "${arg}" -C "${tmpdir}"
            echo "== ${arg} =="
            while IFS= read -r inner; do
                check_one "${inner}"
            done < <(find "${tmpdir}" -type f -perm -u+x ! -name '*.sh' ! -name '*.md' | sort)
            rm -rf "${tmpdir}"
            tmpdir=""
            ;;
        *)
            check_one "${arg}"
            ;;
    esac
done

if [[ "${fail}" -ne 0 ]]; then
    echo
    echo "At least one binary needs shared libraries at run time."
    echo "On Linux that is usually a missing -static in the target's LINK_OPTS."
    exit 1
fi

echo
echo "All checked binaries are self-contained."
