load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Factory for tree-sitter grammar repositories. Each grammar is a generated
# parser.c (sometimes a scanner.c) plus a few headers from the official
# tree-sitter-<lang> GitHub release. Pinned by tag + sha256 for hermetic builds.
#
# The generated BUILD file exposes a single cc_library named after the repo,
# linking the tree-sitter core from the BCR module. Highlights queries are NOT
# taken from these archives: they live under //src/common:highlight_queries and
# are embedded at build time (see //src/common:syntax), so a query can be tuned
# or patched without refetching the grammar.
def grammar_repo(name, version, sha256, has_scanner = False, aliases = [], org = "tree-sitter", extra_srcs = []):
    srcs = ["src/parser.c", "src/tree_sitter/parser.h",
            "src/tree_sitter/alloc.h", "src/tree_sitter/array.h"]
    if has_scanner:
        srcs.append("src/scanner.c")
    srcs.extend(extra_srcs)
    srcs_literal = "[\n"
    for s in srcs:
        srcs_literal += '        "%s",\n' % s
    srcs_literal += "    ]"
    # Generated scanners (cpp/python/bash) use C11's static_assert; the host
    # builds with --conlyopt=-std=gnu99, so these translation units need c11.
    # MSVC has no -std flag and accepts static_assert natively, hence select.
    build_content = """
cc_library(
    name = "{name}",
    srcs = {srcs},
    includes = ["src"],
    copts = select({{
        "@edgeclaw//:windows": [],
        "//conditions:default": ["-std=c11"],
    }}),
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)
""".format(name = name, srcs = srcs_literal)
    http_archive(
        name = name,
        build_file_content = build_content,
        sha256 = sha256,
        strip_prefix = "tree-sitter-%s-%s" % (name.replace("tree-sitter-", ""), version),
        urls = ["https://github.com/%s/tree-sitter-%s/archive/refs/tags/v%s.tar.gz" % (
            org, name.replace("tree-sitter-", ""), version)],
    )
