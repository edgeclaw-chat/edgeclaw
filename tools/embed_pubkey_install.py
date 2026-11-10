#!/usr/bin/env python3
"""Embed tools/update_signing.pub.pem into the installers.

Both installers are fetched standalone (`curl | sh`, `iwr | iex`) with no checkout, so each
has to carry the public key inline. That makes them a third and fourth copy of a value that
already lives in the PEM and in the compiled `src/common/edc_update_pubkey.c`.

Four copies of one key is three too many, but the alternatives are worse: downloading the key
would make it only as trustworthy as the channel that already carries the script, and skipping
verification is not an option. So the copies are kept in step by a command rather than by
memory, and `tools/update_keygen.sh --check` reports whether they agree.

Usage: python3 tools/embed_pubkey_install.py [--check]
"""
import pathlib
import sys

PEM = pathlib.Path("tools/update_signing.pub.pem")

# (script, opening marker line, closing marker line). The two shells delimit here-documents
# differently, which is the only reason this is a table rather than one call.
TARGETS = [
    ("tools/install.sh", "<<'EDGECLAW_UPDATE_PUBKEY'\n", "\nEDGECLAW_UPDATE_PUBKEY\n"),
    ("tools/install.ps1", "@'\n", "\n'@\n"),
]

BEGIN = "EDGECLAW_UPDATE_PUBKEY"  # the shell heredoc tag; also what the ps1 comment names


def embed(path: pathlib.Path, opening: str, closing: str, pem: str, check_only: bool) -> bool:
    """Returns True when the file already holds the PEM (or was just updated)."""
    text = path.read_text(encoding="utf-8")
    start = text.find(opening)
    if start < 0:
        raise SystemExit(f"{path}: could not find the opening marker {opening!r}")
    body_start = start + len(opening)
    end = text.find(closing, body_start)
    if end < 0:
        raise SystemExit(f"{path}: could not find the closing marker {closing!r}")

    if check_only:
        return text[body_start:end].strip() == pem

    path.write_text(text[:body_start] + pem + text[end:], encoding="utf-8")
    return True


def main() -> int:
    check_only = "--check" in sys.argv[1:]

    pem = PEM.read_text(encoding="utf-8").strip()
    if "BEGIN PUBLIC KEY" not in pem:
        raise SystemExit(f"{PEM} is not a PEM public key")

    drifted = []
    for name, opening, closing in TARGETS:
        path = pathlib.Path(name)
        if not path.exists():
            # install.ps1 may not exist yet in a tree that predates it; not an error.
            print(f"skip {name} (not present)")
            continue
        ok = embed(path, opening, closing, pem, check_only)
        if check_only:
            if ok:
                print(f"MATCH: {name} embeds the committed public key")
            else:
                print(f"MISMATCH: {name} embeds a different public key")
                drifted.append(name)
        else:
            print(f"embedded {PEM} into {name}")

    if drifted:
        print("  run: python3 tools/embed_pubkey_install.py")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
