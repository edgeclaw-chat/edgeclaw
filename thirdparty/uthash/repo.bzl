load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def repo():
    http_archive(
        name = "uthash",
        urls = ["https://github.com/troydhanson/uthash/archive/refs/tags/v2.3.0.tar.gz"],
        strip_prefix = "uthash-2.3.0",
        build_file = "//thirdparty/uthash:uthash.BUILD",
    )
