#!/usr/bin/env python3
"""Fail if a Windows binary imports a C/C++ runtime DLL the release archive does not ship.

The archive carries one file: edgeclaw.exe. Anything the exe needs from outside Windows
itself is therefore missing on a clean machine, and the failure looks like

    The code execution cannot proceed because libstdc++-6.dll was not found

which the user sees instead of a program. Two different toolchains can produce that mistake:

  * MinGW, unless the link is static: libstdc++-6.dll, libgcc_s_seh-1.dll, libwinpthread-1.dll
  * MSVC, unless the CRT is static (/MT, Bazel's static_link_msvcrt): VCRUNTIME140.dll,
    MSVCP140.dll, and the UCRT api-ms-win-crt-* set plus ucrtbase.dll

Both are rejected here, so the gate keeps working whichever compiler the Windows build uses,
and switching compilers cannot silently turn it into a no-op (which is what happened when the
naming assumption in the old objdump-based shell check stopped matching reality).

The import list is read straight out of the PE import directory rather than by asking a
toolchain tool to print it: no dumpbin, no vcvars, no objdump on PATH, and no dependence on
x86_64-w64-mingw32-objdump existing on a machine that no longer builds with MinGW. It also
means this check reads what the LINKER produced, not what a BUILD file was supposed to pass.

Usage: tools/check_windows_imports.py <path-to-exe> [--quiet]
Exit: 0 = self-contained, 1 = a runtime DLL is imported, 2 = could not read the file.
"""

import struct
import sys

# DLLs that must NOT appear. Matched case-insensitively as prefixes / families:
#   vcruntime140.dll, vcruntime140_1.dll, msvcp140.dll, msvcp140_1.dll, msvcp140_2.dll,
#   msvcp140_atomic_wait.dll, msvcp140_codecvt_ids.dll, concrt140.dll, msvcr120.dll,
#   ucrtbase.dll, the whole api-ms-win-crt-* api-set, and the MinGW trio.
RUNTIME_PREFIXES = (
    "vcruntime",
    "msvcp",
    "msvcr",
    "concrt",
    "ucrtbase",
    "libstdc++",
    "libgcc",
    "libwinpthread",
)
RUNTIME_API_SET_PREFIX = "api-ms-win-crt-"


def read_imports(path):
    """Return the DLL names in the PE import directory of |path|."""
    with open(path, "rb") as fh:
        data = fh.read()

    if data[:2] != b"MZ":
        raise ValueError("not a PE file (no MZ header)")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError("not a PE file (no PE signature)")

    coff = pe_off + 4
    nsections, = struct.unpack_from("<H", data, coff + 2)
    opt_size, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)

    # Data directories start after the fixed part of the optional header: 96 bytes for
    # PE32, 112 for PE32+. The import directory is entry 1.
    if magic == 0x10B:        # PE32
        dd = opt + 96
    elif magic == 0x20B:      # PE32+
        dd = opt + 112
    else:
        raise ValueError("unknown optional-header magic 0x%x" % magic)

    import_rva, = struct.unpack_from("<I", data, dd + 8)
    if import_rva == 0:
        return []

    # Section table, to translate an RVA into a file offset.
    sections = []
    sec = opt + opt_size
    for i in range(nsections):
        base = sec + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, base + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def to_offset(rva):
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rawptr + (rva - vaddr)
        raise ValueError("RVA 0x%x is not inside any section" % rva)

    names = []
    desc = to_offset(import_rva)
    while True:
        # IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk, TimeDateStamp, ForwarderChain,
        # Name, FirstThunk -- 20 bytes; a zeroed Name ends the array.
        name_rva, = struct.unpack_from("<I", data, desc + 12)
        if name_rva == 0:
            break
        off = to_offset(name_rva)
        end = data.index(b"\0", off)
        names.append(data[off:end].decode("ascii", "replace"))
        desc += 20
    return names


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: %s <path-to-exe> [--quiet]\n" % argv[0])
        return 2
    path = argv[1]
    quiet = "--quiet" in argv[2:]

    try:
        imports = read_imports(path)
    except Exception as exc:                                  # noqa: BLE001 - report, do not raise
        sys.stderr.write("error: cannot read the import table of %s: %s\n" % (path, exc))
        return 2

    if not quiet:
        print("--- DLL imports of %s ---" % path)
        for name in imports:
            print("  " + name)
        print("--- %d import(s) ---" % len(imports))

    bad = []
    for name in imports:
        low = name.lower()
        if low.startswith(RUNTIME_API_SET_PREFIX) or low.startswith(RUNTIME_PREFIXES):
            bad.append(name)

    if bad:
        print("::error::%s needs runtime DLLs the archive does not ship: %s"
              % (path, ", ".join(bad)), file=sys.stderr)
        print("::error::the Windows link is not self-contained: pass /MT "
              "(Bazel: --features=static_link_msvcrt) for MSVC, or -static for MinGW",
              file=sys.stderr)
        return 1

    if not quiet:
        print("ok: no C/C++ runtime DLL imports -- the exe is self-contained")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
