load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# tree-sitter C grammar. Used by the syntax-highlighting engine (see
# //src/common:syntax). Pinned to a release tag with its sha256 so the build is
# hermetic and reproducible. The grammar is a single generated parser.c with no
# external scanner; tree-sitter-c.BUILD wraps it as a cc_library that links the
# tree-sitter core from the BCR module.
def repo():
    http_archive(
        name = "tree-sitter-c",
        build_file = "//thirdparty/tree-sitter-c:tree-sitter-c.BUILD",
        sha256 = "b66c5043e26d84e5f17a059af71b157bcf202221069ed220aa1696d7d1d28a7a",
        strip_prefix = "tree-sitter-c-0.23.4",
        urls = [
            "https://github.com/tree-sitter/tree-sitter-c/archive/refs/tags/v0.23.4.tar.gz",
        ],
    )
