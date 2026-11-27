load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# The tree-sitter-typescript release contains TWO grammars side by side:
# typescript/ (tree_sitter_typescript) and tsx/ (tree_sitter_tsx), sharing a
# common/scanner.h. The generic grammar_repo factory assumes one grammar at
# src/, so TypeScript needs its own repo producing two cc_library targets.
# Both scanners include "../../common/scanner.h" relative to their own path,
# which resolves correctly against the archive layout below.
def repo():
    http_archive(
        name = "tree-sitter-typescript",
        build_file_content = """
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "tree-sitter-typescript",
    srcs = [
        "typescript/src/parser.c",
        "typescript/src/scanner.c",
        "typescript/src/tree_sitter/parser.h",
        "typescript/src/tree_sitter/alloc.h",
        "typescript/src/tree_sitter/array.h",
        "common/scanner.h",
    ],
    includes = ["typescript/src"],
    copts = select({
        "@edgeclaw//:windows": [],
        "//conditions:default": ["-std=c11"],
    }),
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)

cc_library(
    name = "tree-sitter-tsx",
    srcs = [
        "tsx/src/parser.c",
        "tsx/src/scanner.c",
        "tsx/src/tree_sitter/parser.h",
        "tsx/src/tree_sitter/alloc.h",
        "tsx/src/tree_sitter/array.h",
        "common/scanner.h",
    ],
    includes = ["tsx/src"],
    copts = select({
        "@edgeclaw//:windows": [],
        "//conditions:default": ["-std=c11"],
    }),
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)
""",
        sha256 = "2c4ce711ae8d1218a3b2f899189298159d672870b5b34dff5d937bed2f3e8983",
        strip_prefix = "tree-sitter-typescript-0.23.2",
        urls = ["https://github.com/tree-sitter/tree-sitter-typescript/archive/refs/tags/v0.23.2.tar.gz"],
    )
