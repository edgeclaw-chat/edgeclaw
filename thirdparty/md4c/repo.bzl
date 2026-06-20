load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "md4c",
        remote = "https://github.com/mity/md4c.git",
        commit = "729e6b8b320caa96328968ab27d7db2235e4fb47",
        build_file = "//thirdparty/md4c:md4c.BUILD",
    )
