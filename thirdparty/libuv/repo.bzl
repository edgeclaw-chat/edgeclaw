load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "libuv",
        remote = "https://github.com/libuv/libuv.git",
        tag = "v1.52.1",
        build_file = "//thirdparty/libuv:libuv.BUILD",
    )
