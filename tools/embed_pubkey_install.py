#!/usr/bin/env python3
"""Embed the release signing public key into the installers.

Both installers are fetched standalone (`curl | sh`, `iwr | iex`) with no checkout, so each
has to carry the public key inline. That makes them further copies of a value that already
lives in the PEM and in the compiled `src/common/edc_update_pubkey.c`.

Several copies of one key is too many, but the alternatives are worse: downloading the key
would make it only as trustworthy as the channel that already carries the script, and skipping
verification is not an option. So the copies are kept in step by a command rather than by
memory, and `tools/update_keygen.sh --check` reports whether they agree.

The two scripts take the key in different forms, for a reason:

  install.sh   PEM text. openssl reads it directly; no parsing to write.
  install.ps1  the raw 65-byte point, hex. Windows PowerShell 5.1 -- what "powershell"
               resolves to on a stock Windows 10/11 -- runs on .NET Framework, which has no
               ECDsa.ImportFromPem(). ECParameters/ImportParameters does exist there, and it
               wants the point, not the PEM. It is also the same 65 bytes the C verifier
               uses, so the two agree by construction rather than by both parsing ASN.1.

Usage: python3 tools/embed_pubkey_install.py [--check]
"""
import pathlib
import subprocess
import sys

PEM = pathlib.Path("tools/update_signing.pub.pem")
SH = "tools/install.sh"
PS1 = "tools/install.ps1"

BEGIN = "EDGECLAW_UPDATE_PUBKEY"


def raw_point(pem_path: pathlib.Path) -> bytes:
    """The uncompressed P-256 point: 0x04 || X || Y."""
    der = subprocess.run(
        ["openssl", "pkey", "-pubin", "-in", str(pem_path), "-outform", "DER"],
        capture_output=True, check=True,
    ).stdout
    marker = b"\x03\x42\x00\x04"
    idx = der.rfind(marker)
    if idx < 0:
        raise SystemExit("could not locate an uncompressed P-256 point in the key")
    point = der[idx + 3: idx + 3 + 65]
    if len(point) != 65 or point[0] != 0x04:
        raise SystemExit("extracted point is not a 65-byte uncompressed EC point")
    return point


def splice(path: pathlib.Path, opening: str, closing: str, body: str, check_only: bool) -> bool:
    """Returns True when the file already holds |body| (or was just updated)."""
    text = path.read_text(encoding="utf-8")
    start = text.find(opening)
    if start < 0:
        raise SystemExit(f"{path}: could not find the opening marker {opening!r}")
    body_start = start + len(opening)
    end = text.find(closing, body_start)
    if end < 0:
        raise SystemExit(f"{path}: could not find the closing marker {closing!r}")

    if check_only:
        return text[body_start:end].strip() == body

    path.write_text(text[:body_start] + body + text[end:], encoding="utf-8")
    return True


def main() -> int:
    check_only = "--check" in sys.argv[1:]

    pem_text = PEM.read_text(encoding="utf-8").strip()
    if "BEGIN PUBLIC KEY" not in pem_text:
        raise SystemExit(f"{PEM} is not a PEM public key")
    hex_point = raw_point(PEM).hex()

    # (path, opening marker, closing marker, body)
    jobs = [
        (pathlib.Path(SH), f"<<'{BEGIN}'\n", f"\n{BEGIN}\n", pem_text),
        (pathlib.Path(PS1), "@'\n", "\n'@\n", hex_point),
    ]

    drifted = []
    for path, opening, closing, body in jobs:
        if not path.exists():
            print(f"skip {path} (not present)")
            continue
        ok = splice(path, opening, closing, body, check_only)
        if check_only:
            if ok:
                print(f"MATCH: {path} embeds the committed public key")
            else:
                print(f"MISMATCH: {path} embeds a different public key")
                drifted.append(str(path))
        else:
            print(f"embedded the committed public key into {path}")

    if drifted:
        print("  run: python3 tools/embed_pubkey_install.py")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
