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
