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
def grammar_repo(name, version, sha256, has_scanner = False, aliases = [], org = "tree-sitter", extra_srcs = [], extra_copts = [], src_dir = "src", tag = None, extra_hdrs = []):
    # `tag` overrides the URL tag when a release has no "v" prefix.
    tag = tag if tag != None else "v" + version
    srcs = [src_dir + "/parser.c", src_dir + "/tree_sitter/parser.h",
            src_dir + "/tree_sitter/alloc.h", src_dir + "/tree_sitter/array.h"]
    if has_scanner:
        srcs.append(src_dir + "/scanner.c")
    # A file #included by scanner.c must be a header (so it lands in every
    # CppCompile sandbox). Some grammars #include a ".c" as a schema fragment.
    hdrs = [src_dir + "/tree_sitter/parser.h", src_dir + "/tree_sitter/alloc.h",
            src_dir + "/tree_sitter/array.h"] + extra_hdrs
    srcs.extend([s for s in extra_srcs if s not in hdrs])
    srcs_literal = "[\n"
    for s in srcs:
        srcs_literal += '        "%s",\n' % s
    srcs_literal += "    ]"
    hdrs_literal = "[\n"
    for h in hdrs:
        hdrs_literal += '        "%s",\n' % h
    hdrs_literal += "    ]"
    copts_literal = "[\n        \"-std=c11\",\n"
    for c in extra_copts:
        copts_literal += '        "%s",\n' % c
    copts_literal += "    ]"
    # Generated scanners (cpp/python/bash) use C11's static_assert; the host
    # builds with --conlyopt=-std=gnu99, so these translation units need c11.
    # MSVC has no -std flag and accepts static_assert natively, hence select.
    build_content = """
cc_library(
    name = "{name}",
    srcs = {srcs},
    hdrs = {hdrs},
    includes = ["{src_dir}"],
    copts = select({{
        "@edgeclaw//:windows": [],
        "//conditions:default": {copts},
    }}),
    visibility = ["//visibility:public"],
    deps = ["@tree-sitter-bazel//:tree-sitter"],
)
""".format(name = name, srcs = srcs_literal, hdrs = hdrs_literal, copts = copts_literal, src_dir = src_dir)
    http_archive(
        name = name,
        build_file_content = build_content,
        sha256 = sha256,
        strip_prefix = "tree-sitter-%s-%s" % (name.replace("tree-sitter-", ""), version),
        urls = ["https://github.com/%s/tree-sitter-%s/archive/refs/tags/%s.tar.gz" % (
            org, name.replace("tree-sitter-", ""), tag)],
    )
