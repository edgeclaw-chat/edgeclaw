"""Build-time generation of the Swift tree-sitter grammar.

The upstream repo (alex-pinkus/tree-sitter-swift@0.7.1) does not commit
src/parser.c; it is generated from grammar.js by tree-sitter CLI. Instead of
vendoring the ~18 MB generated C file, we download the pinned CLI binary for the
BUILD machine's OS/arch from the tree-sitter GitHub release and run
`tree-sitter generate --abi 15` at repository-fetch time. The output is
byte-identical to the vendored file (verified by sha256).

Both the grammar tarball and the CLI binary are pinned by sha256; generation is
deterministic, so the result is hermetic and reproducible. parser.c is
platform-independent, so cross-compiling to musl uses the build machine's CLI.
"""

_CLI_VERSION = "0.26.5"
# (os.name, os.arch) -> (release asset, sha256 of the .gz download)
_CLI_ASSETS = {
    ("linux", "x86_64"): ("tree-sitter-linux-x64.gz", "d38d9a22ef398489e1eb291b2dea41467487020fe8280c2311dbbad9ba663a34"),
    ("linux", "aarch64"): ("tree-sitter-linux-arm64.gz", "519e8648004a725a3bb566bdb3f3134946df4c9d7fcda6be5cf67d237d2b0921"),
    ("macos", "x86_64"): ("tree-sitter-macos-x64.gz", "ca61ceaf1193fb665d40300db85c35476c2a503bf0d37fd8af786dcf86e4f79c"),
    ("macos", "aarch64"): ("tree-sitter-macos-arm64.gz", "c6cf9a8e77e01df49175aec102c55b294f26627ee73effe025cf40764c6c6b24"),
    ("windows", "x86_64"): ("tree-sitter-windows-x64.gz", "0adfa6a81fd94d3a8eda9d9c89333bf1263bc112c7797dc90bea80b519fe2181"),
}

_GRAMMAR_URL = "https://codeload.github.com/alex-pinkus/tree-sitter-swift/tar.gz/refs/tags/0.7.1"
_GRAMMAR_SHA = "e3bcd98c07243e45ee1e85c3c03359cd38a85645544d8a3d5678076ec5f8c772"


def _swift_grammar_repo_impl(repository_ctx):
    # Bazel's os.name/arch use platform-specific spellings: "mac os x" vs
    # "macos", "windows server 2022" vs "windows", "amd64" vs "x86_64".
    # Normalise both before looking up the pinned CLI asset.
    raw_os = repository_ctx.os.name
    raw_arch = repository_ctx.os.arch
    if raw_os.startswith("mac os") or raw_os.startswith("macos"):
        os_name = "macos"
    elif raw_os.startswith("windows"):
        os_name = "windows"
    elif raw_os.startswith("linux"):
        os_name = "linux"
    else:
        os_name = raw_os
    if raw_arch in ("amd64", "x86_64", "x64"):
        arch = "x86_64"
    elif raw_arch in ("aarch64", "arm64"):
        arch = "aarch64"
    else:
        arch = raw_arch
    asset = _CLI_ASSETS.get((os_name, arch))
    if asset == None:
        fail("tree-sitter-swift: no CLI asset for build machine %s/%s (raw %s/%s)" %
             (os_name, arch, raw_os, raw_arch))

    # 1. Grammar tarball, pinned.
    grammar = repository_ctx.path("grammar.tar.gz")
    repository_ctx.download(_GRAMMAR_URL, grammar, sha256 = _GRAMMAR_SHA)
    repository_ctx.extract(grammar, stripPrefix = "tree-sitter-swift-0.7.1")

    # 2. CLI binary for the BUILD machine, pinned. Release assets are single-file
    #    gzip streams; decompress with the system gzip (present in Git bash on CI).
    cli_gz = repository_ctx.path("tree-sitter-cli.gz")
    cli_url = "https://github.com/tree-sitter/tree-sitter/releases/download/v%s/%s" % (_CLI_VERSION, asset[0])
    repository_ctx.download(cli_url, cli_gz, sha256 = asset[1])
    cli_name = "tree-sitter.exe" if os_name == "windows" else "tree-sitter"
    # Shell redirection keeps the ~21 MB binary out of Starlark strings.
    result = repository_ctx.execute(
        ["sh", "-c", "gzip -dc '%s' > '%s'" % (str(cli_gz), cli_name)])
    if result.return_code != 0:
        fail("gzip decompression failed: %s" % result.stderr)
    if os_name != "windows":
        repository_ctx.execute(["chmod", "+x", cli_name])

    # 3. Generate parser.c in the checkout root (grammar.js lives there).
    gen = repository_ctx.execute(["./" + cli_name, "generate", "--abi", "15"],
                                 working_directory = ".")
    if gen.return_code != 0:
        fail("tree-sitter generate failed: %s" % gen.stderr)

    # 3b. Upstream bug in tree-sitter-swift@0.7.1: the external scanner creates
    #     its state with calloc(0, sizeof(...)), a zero-byte allocation. ASan
    #     catches the subsequent 4-byte field access as a heap overflow. Allocate
    #     one element instead; semantics are identical for a non-ASan build.
    scanner_path = repository_ctx.path("src/scanner.c")
    scanner = repository_ctx.read(scanner_path)
    old = "calloc(0, sizeof(struct ScannerState))"
    if old not in scanner:
        fail("tree-sitter-swift scanner.c no longer has the expected calloc(0,...) -- recheck the upstream patch")
    repository_ctx.file(scanner_path,
                        content = scanner.replace(old, "calloc(1, sizeof(struct ScannerState))"))

    # 4. Expose the same target shape the old vendored repo had.
    repository_ctx.file("BUILD.bazel", """
load("@rules_cc//cc:defs.bzl", "cc_library")
cc_library(
    name = "tree-sitter-swift",
    srcs = [
        "src/parser.c",
        "src/scanner.c",
        "src/tree_sitter/parser.h",
        "src/tree_sitter/alloc.h",
        "src/tree_sitter/array.h",
    ],
    includes = ["src"],
    copts = select({
        "@edgeclaw//:windows": [],
        "//conditions:default": ["-std=c11"],
    }),
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)
""")


swift_grammar_repo = repository_rule(
    implementation = _swift_grammar_repo_impl,
    local = True,  # depends on the build machine's OS/arch
)
