# Vendored tree-sitter-swift grammar (generated parser.c)

Unlike most tree-sitter grammars, `alex-pinkus/tree-sitter-swift` does NOT
commit `src/parser.c` to its git repository — it only ships `grammar.js`,
`src/grammar.json`, `src/scanner.c` and the headers. Its releases provide a
precompiled WASM module (for web-tree-sitter consumers like opencode), which we
cannot use: our highlighter is the native C tree-sitter library from BCR and
needs `parser.c` source.

So `src/parser.c` here is **generated locally and vendored** as a fixed source
file. This is a grammar source, not the tree-sitter core (the core still comes
from BCR via `bazel_dep(tree-sitter-bazel)`).

Provenance / regeneration:
- upstream: https://github.com/alex-pinkus/tree-sitter-swift at tag `0.7.1`
- generated with `tree-sitter-cli` **0.26.5** (same version as the core lib):
  ```
  cd <checkout of tree-sitter-swift@0.7.1>
  tree-sitter generate --abi 15      # writes src/parser.c (LANGUAGE_VERSION 15)
  ```
- `scanner.c` and `src/tree_sitter/*.h` come verbatim from that tag.
- generation is deterministic: two runs produced byte-identical `parser.c`
  (sha256 af17e2ccfa504cd3...).
- The CLI is a one-off local tool and is NOT part of the build/CI; only the
  generated C source is in the repo, so builds stay offline and reproducible.
