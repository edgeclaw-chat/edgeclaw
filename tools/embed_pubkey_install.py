#!/usr/bin/env python3
"""Embed tools/update_signing.pub.pem into tools/install.sh.

`install.sh` is run as `curl ... | sh`, so it has no checkout and cannot read the public key
from disk. The key therefore has to live inside the script -- between the
EDGECLAW_UPDATE_PUBKEY markers -- which makes it a third copy that can drift from the two
authoritative ones (`tools/update_signing.pub.pem` and the compiled
`src/common/edc_update_pubkey.c`).

Three copies of one key is two too many, but the alternatives are worse: downloading the
key would make it as trustworthy as the channel that already carries the script, and
skipping verification is not an option. So the copies are kept in step by a command rather
than by memory, and `tools/update_keygen.sh --check` prints whether they agree.

Usage: python3 tools/embed_pubkey_install.py [--check]
"""
import pathlib
import sys

PEM = pathlib.Path("tools/update_signing.pub.pem")
SCRIPT = pathlib.Path("tools/install.sh")
BEGIN = "EDGECLAW_UPDATE_PUBKEY"
END = "EDGECLAW_UPDATE_PUBKEY"


def main() -> int:
    check_only = "--check" in sys.argv[1:]

    pem = PEM.read_text(encoding="utf-8").strip()
    if "BEGIN PUBLIC KEY" not in pem:
        raise SystemExit(f"{PEM} is not a PEM public key")

    text = SCRIPT.read_text(encoding="utf-8")
    start = text.find(f"<<'{BEGIN}'\n")
    if start < 0:
        raise SystemExit(f"{SCRIPT}: could not find the <<'{BEGIN}' heredoc")
    body_start = start + len(f"<<'{BEGIN}'\n")
    end = text.find(f"\n{END}\n", body_start)
    if end < 0:
        raise SystemExit(f"{SCRIPT}: could not find the closing {END} line")

    current = text[body_start:end]
    if check_only:
        if current.strip() == pem:
            print("MATCH: install.sh embeds the committed public key")
            return 0
        print("MISMATCH: install.sh embeds a different public key")
        print("  run: python3 tools/embed_pubkey_install.py")
        return 1

    SCRIPT.write_text(text[:body_start] + pem + text[end:], encoding="utf-8")
    print(f"embedded {PEM} into {SCRIPT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
