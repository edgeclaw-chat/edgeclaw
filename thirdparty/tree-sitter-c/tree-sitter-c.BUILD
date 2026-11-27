cc_library(
    name = "tree-sitter-c",
    srcs = [
        "src/parser.c",
        "src/tree_sitter/parser.h",
        "src/tree_sitter/alloc.h",
        "src/tree_sitter/array.h",
    ],
    # parser.c includes "tree_sitter/parser.h"; the include root is src/.
    includes = ["src"],
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)
