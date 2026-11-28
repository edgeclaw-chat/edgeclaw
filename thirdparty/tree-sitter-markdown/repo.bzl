load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# tree-sitter-grammars/tree-sitter-markdown ships TWO grammars side by side:
# tree-sitter-markdown/ (tree_sitter_markdown) and tree-sitter-markdown-inline/
# (tree_sitter_markdown_inline). The generic grammar_repo factory assumes one
# grammar at src/, so markdown gets its own repo producing two cc_library
# targets from one archive (same pattern as tree-sitter-typescript).
def repo():
    http_archive(
        name = "tree-sitter-markdown",
        build_file_content = """
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "tree-sitter-markdown",
    srcs = [
        "tree-sitter-markdown/src/parser.c",
        "tree-sitter-markdown/src/scanner.c",
        "tree-sitter-markdown/src/tree_sitter/parser.h",
        "tree-sitter-markdown/src/tree_sitter/alloc.h",
        "tree-sitter-markdown/src/tree_sitter/array.h",
    ],
    includes = ["tree-sitter-markdown/src"],
    copts = select({
        "@edgeclaw//:windows": [],
        "//conditions:default": ["-std=c11"],
    }),
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)

cc_library(
    name = "tree-sitter-markdown-inline",
    srcs = [
        "tree-sitter-markdown-inline/src/parser.c",
        "tree-sitter-markdown-inline/src/scanner.c",
        "tree-sitter-markdown-inline/src/tree_sitter/parser.h",
        "tree-sitter-markdown-inline/src/tree_sitter/alloc.h",
        "tree-sitter-markdown-inline/src/tree_sitter/array.h",
    ],
    includes = ["tree-sitter-markdown-inline/src"],
    copts = select({
        "@edgeclaw//:windows": [],
        "//conditions:default": ["-std=c11"],
    }),
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)
""",
        sha256 = "481ce2c1cce7b1130bc8455c186052bd0fa461f1ff488666994c6a3da7fca2ac",
        strip_prefix = "tree-sitter-markdown-0.5.4",
        urls = ["https://github.com/tree-sitter-grammars/tree-sitter-markdown/archive/refs/tags/v0.5.4.tar.gz"],
    )
