#!/usr/bin/env python3
"""Regenerate THIRD_PARTY_NOTICES.md and licenses/ from the components we ship.

WHY THE TEXTS ARE COMMITTED RATHER THAN GENERATED DURING THE BUILD: EdgeClaw ships as one
statically linked binary, so distributing it distributes every library linked into it.
Those licences must travel with the tarball. Reading them out of Bazel's external
repositories at build time would make the release depend on a cache directory whose path
changes per output base -- the notice would be missing exactly when someone builds from a
clean machine. So the texts live in the repository, and this script is how they get there.

`--check` compares the committed files against what the script would write, so CI can fail
when a dependency is bumped without its licence being refreshed.

    tools/gen_notices.py            # refresh licenses/ and THIRD_PARTY_NOTICES.md
    tools/gen_notices.py --check    # verify they are current (exit 1 if not)

WHERE EACH TEXT COMES FROM, and why there are three kinds:

  external  the component is a Bazel module or repository rule; its licence is a file in
            the fetched repository. Located by globbing the Bazel external roots, because
            the mangled directory name (e.g. "+bc_thirdparty_deps+libuv") is an
            implementation detail of the module extension.
  vendored  the source lives in this repository (thirdparty/utf8proc). The licence is the
            comment block at the top of the file, which is where upstream puts it.
  embedded  there is no licence file to copy: SQLite is public domain by dedication and
            the IANA timezone database is public domain by policy. Their statements are
            reproduced here verbatim rather than linked, because a notice that points at
            a URL is not a notice when the reader is offline.
"""

import argparse
import glob
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
LICENSES_DIR = os.path.join(REPO, "licenses")
NOTICES = os.path.join(REPO, "THIRD_PARTY_NOTICES.md")

SQLITE_TEXT = """\
The author disclaims copyright to this source code.  In place of a legal notice, here is
a blessing:

   May you do good and not evil.
   May you find forgiveness for yourself and forgive others.
   May you share freely, never taking more than you give.

SQLite is in the public domain.  See https://www.sqlite.org/copyright.html
"""

TZDB_TEXT = """\
The IANA time zone database (tzdata/tzdb) is in the public domain.

The files under thirdparty/brutezone/ are generated from that database: timezone_database.h
is a compiled table of the zone rules, and timezone.c/timezone.h are a small lookup layer
written for EdgeClaw.  The database is maintained by the IANA time zone database project at
https://www.iana.org/time-zones; its public-domain status is stated in the tz repository's
LICENSE file ("This file is in the public domain, so clarified as of 2009-05-17 by Arthur
David Olson.").
"""

# name, spdx, project url, kind, source detail, note
#
# `note` is shown to the reader and should say what the component is FOR, not repeat the
# licence name -- someone reading a notice wants to know why the file is there.
COMPONENTS = [
    dict(name="ccronexpr", spdx="Apache-2.0",
         url="https://github.com/exander77/supertinycron",
         kind="external", path="+bc_thirdparty_deps+ccronexpr", file="LICENSE.txt",
         note="cron expression parsing for the crontab tool"),
    dict(name="libuv", spdx="MIT",
         url="https://github.com/libuv/libuv",
         kind="external", path="+bc_thirdparty_deps+libuv", file="LICENSE",
         extra=["LICENSE-docs", "LICENSE-extra"],
         note="the event loop every actor mailbox runs on"),
    dict(name="lwip", spdx="BSD-3-Clause",
         url="https://github.com/lwip-tcpip/lwip",
         kind="external", path="+bc_thirdparty_deps+lwip", file="COPYING",
         note="TCP/IP stack for the PPPoS remote-device transport"),
    dict(name="md4c", spdx="MIT",
         url="https://github.com/mity/md4c",
         kind="external", path="+bc_thirdparty_deps+md4c", file="LICENSE.md",
         note="markdown rendering in the TUI"),
    dict(name="sds", spdx="BSD-2-Clause",
         url="https://github.com/antirez/sds",
         kind="external", path="+bc_thirdparty_deps+sds", file="LICENSE",
         note="dynamic strings"),
    dict(name="uthash", spdx="BSD-1-Clause",
         url="https://github.com/troydhanson/uthash",
         kind="external", path="+bc_thirdparty_deps+uthash", file="LICENSE",
         note="hash tables and linked-list macros"),
    dict(name="llhttp", spdx="MIT",
         url="https://github.com/nodejs/llhttp",
         kind="external", path="+bc_thirdparty_deps+llhttp", file="LICENSE",
         extra=["LICENSE-MIT"],
         note="HTTP response parsing for the HTTPS client"),
    dict(name="libyuarel", spdx="MIT",
         url="https://github.com/jacketizer/libyuarel",
         kind="external", path="+bc_thirdparty_deps+libyuarel", file="LICENSE",
         note="URL parsing"),
    dict(name="log.c", spdx="MIT",
         url="https://github.com/rxi/log.c",
         kind="external", path="+bc_thirdparty_deps+log.c", file="LICENSE",
         note="logging"),
    dict(name="mbedtls", spdx="Apache-2.0", spdx_note="dual-licensed Apache-2.0 OR GPL-2.0-or-later; EdgeClaw takes it under Apache-2.0",
         url="https://github.com/Mbed-TLS/mbedtls",
         kind="external", path="mbedtls+", file="LICENSE",
         note="TLS, AES-GCM and HKDF (config encryption, HTTPS)"),
    dict(name="cjson", spdx="MIT",
         url="https://github.com/DaveGamble/cJSON",
         kind="external", path="cjson+", file="LICENSE",
         note="JSON for the wire protocol and persisted state"),
    dict(name="ftxui", spdx="MIT",
         url="https://github.com/ArthurSonzogni/FTXUI",
         kind="external", path="ftxui+", file="LICENSE",
         note="the terminal UI"),
    dict(name="sqlite3", spdx="Public Domain",
         url="https://www.sqlite.org/",
         kind="embedded", text=SQLITE_TEXT,
         note="the session state store"),
    dict(name="utf8proc", spdx="MIT",
         url="https://github.com/JuliaStrings/utf8proc",
         kind="vendored", path="thirdparty/utf8proc/utf8proc.c",
         note="UTF-8 validation and grapheme handling"),
    dict(name="iana-tzdb", spdx="Public Domain",
         url="https://www.iana.org/time-zones",
         kind="embedded", text=TZDB_TEXT,
         note="timezone rules behind thirdparty/brutezone"),
]


def external_roots():
    """Every Bazel external root on this machine.

    Several can exist (one per output base). Any of them that still holds the component is
    fine -- the licence text does not depend on which build directory fetched it.

    The layout is ~/.cache/bazel/_bazel_<user>/<output-base-hash>/external, hence two
    wildcards between the bazel root and `external`.
    """
    home = os.path.expanduser("~")
    pattern = os.path.join(home, ".cache", "bazel", "_bazel_*", "*", "external")
    return sorted(glob.glob(pattern))


def leading_comment(path):
    """The C comment block a file starts with, or None.

    Used for vendored sources, where upstream puts the licence in the file header instead
    of shipping a separate LICENSE file.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return None

    out = []
    started = False
    for line in lines:
        stripped = line.strip()
        if not started:
            # Skip a leading mode/editor line ("/* -*- mode: c; ... -*- */").
            if stripped.startswith("/*") and stripped.endswith("*/") and len(stripped) < 80:
                continue
            if not stripped.startswith("/*"):
                return None
            started = True
        out.append(line)
        if line.rstrip().endswith("*/"):
            return "\n".join(out) + "\n"
    return None


def read_component(comp):
    """Return (main licence text, {extra filename: text})."""
    kind = comp["kind"]

    if kind == "embedded":
        return comp["text"], {}

    if kind == "vendored":
        text = leading_comment(os.path.join(REPO, comp["path"]))
        if not text:
            raise SystemExit(
                f"{comp['name']}: no leading comment block in {comp['path']} -- the vendored "
                f"source moved or lost its header. Fix the manifest entry.")
        return text, {}

    # kind == "external"
    for root in external_roots():
        base = os.path.join(root, comp["path"])
        main = os.path.join(base, comp["file"])
        if os.path.isdir(base) and os.path.isfile(main):
            extras = {}
            for name in comp.get("extra", []):
                path = os.path.join(base, name)
                if os.path.isfile(path):
                    extras[name] = read_text(path)
            return read_text(main), extras

    raise SystemExit(
        f"{comp['name']}: not found under any Bazel external root.\n"
        f"  looked for {comp['path']}/{comp['file']}\n"
        f"  roots: {external_roots() or '(none)'}\n"
        f"  build once with Bazel so the repositories are fetched, then rerun.")


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    # Normalise trailing whitespace so a regenerated file does not churn on a stray newline.
    return text.rstrip() + "\n"


def render_notices(entries):
    lines = [
        "# Third-party notices",
        "",
        "EdgeClaw ships as statically linked binaries, so each release archive redistributes",
        "the third-party code linked into it. Every such component in this source tree is",
        "listed below with its licence; the full text of each licence is in `licenses/`",
        "beside this file.",
        "",
        "The list covers the whole tree, so a given archive may not contain every component",
        "below -- it never contains one that is missing from this list. `device_agent` links",
        "a subset of what the interactive binary does.",
        "",
        "EdgeClaw itself is MIT-licensed (see `LICENSE`).",
        "",
        "| Component | Licence | Used for |",
        "|---|---|---|",
    ]
    for comp, _text, _extras in entries:
        lic = comp["spdx"]
        if comp.get("spdx_note"):
            lic += f" ({comp['spdx_note']})"
        lines.append(f"| [{comp['name']}]({comp['url']}) | {lic} | {comp['note']} |")

    lines += ["", "---", ""]
    for comp, text, extras in entries:
        lines += [
            f"## {comp['name']} ({comp['spdx']})",
            "",
            f"<{comp['url']}>",
            "",
            "```",
            text.rstrip(),
            "```",
            "",
        ]
        for name, extra in extras.items():
            lines += [f"### {comp['name']} — {name}", "", "```", extra.rstrip(), "```", ""]
    return "\n".join(lines)


def collect():
    entries = []
    for comp in COMPONENTS:
        text, extras = read_component(comp)
        entries.append((comp, text, extras))
    return entries


def license_filename(comp):
    """What to call the copied text.

    Always LICENSE, even when upstream calls it COPYING or LICENSE.txt: the file lands in a
    directory named after the component, so the name needs only to be recognisable, and one
    convention makes the archive predictable.
    """
    return "LICENSE"


def plan(entries):
    """{relative path under repo: content} for everything this script owns."""
    files = {}
    for comp, text, extras in entries:
        files[os.path.join("licenses", comp["name"], license_filename(comp))] = text
        for name, extra in extras.items():
            files[os.path.join("licenses", comp["name"], name)] = extra
    files["THIRD_PARTY_NOTICES.md"] = render_notices(entries)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="verify the committed files are current; do not write")
    args = ap.parse_args()

    files = plan(collect())

    if args.check:
        stale = []
        for rel, content in files.items():
            path = os.path.join(REPO, rel)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    if f.read() == content:
                        continue
            except OSError:
                pass
            stale.append(rel)
        if stale:
            print("third-party notices are out of date:", file=sys.stderr)
            for rel in sorted(stale):
                print(f"  {rel}", file=sys.stderr)
            print("\nRun tools/gen_notices.py and commit the result.", file=sys.stderr)
            return 1
        print(f"third-party notices are current ({len(files)} files)")
        return 0

    for rel, content in sorted(files.items()):
        path = os.path.join(REPO, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
    print(f"wrote {len(files)} files: THIRD_PARTY_NOTICES.md + licenses/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
