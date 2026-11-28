# tree-sitter-swift grammar (generated at build time)

The upstream repo `alex-pinkus/tree-sitter-swift@0.7.1` does not commit
`src/parser.c` — it is generated from `grammar.js` by tree-sitter CLI. Its
releases ship a precompiled WASM module (for web-tree-sitter consumers such as
opencode), which our native C tree-sitter engine cannot load.

`repo.bzl` therefore generates `parser.c` at repository-fetch time:
- downloads the grammar tarball, pinned by sha256;
- downloads the tree-sitter CLI 0.26.5 binary for the **build machine**'s
  OS/arch from the pinned GitHub release (sha256 per platform);
- runs `tree-sitter generate --abi 15` to produce `src/parser.c`;
- exposes a `cc_library` target `@tree-sitter-swift//:tree-sitter-swift`.

No generated C is checked in, so the PR/repo stays small. Generation is
byte-for-byte deterministic (verified: the CLI-generated parser.c sha256
matches the previously vendored file, `af17e2cc…`) and both inputs are pinned,
so builds remain reproducible. `parser.c` is platform-independent, so
cross-compiling to musl uses the build machine's CLI without issue.

Upstream patch: `src/scanner.c` in v0.7.1 allocates its state with
`calloc(0, sizeof(struct ScannerState))` (a zero-byte allocation). Under ASan
the subsequent 4-byte field access is a heap-buffer-overflow. The repo rule
rewrites that to `calloc(1, ...)` at fetch time; semantics are unchanged for a
non-sanitized build. If upstream fixes this, remove the patch block in
`repo.bzl` (it fails loudly when the expected text is gone).

Requires `gzip` on the build machine (present in Git bash on Windows CI) and
network access for the two pinned downloads on first fetch (Bazel caches them
afterwards).

To change the grammar version: update `_GRAMMAR_URL`, `_GRAMMAR_SHA`, and the
`stripPrefix` in `repo.bzl`. To change the CLI version: update `_CLI_VERSION`
and the per-platform asset sha256 values.
