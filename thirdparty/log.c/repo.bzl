load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "log.c",
        remote = "https://github.com/rxi/log.c.git",
        commit = "f9ea34994bd58ed342d2245cd4110bb5c6790153",
        build_file = "//thirdparty/log.c:log.c.BUILD"
    )
