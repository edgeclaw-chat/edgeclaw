load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "linenoise",
        remote = "https://github.com/antirez/linenoise.git",
        commit = "a473823d74b93eab2ba83480df16ed37617493f2",
        build_file = "//thirdparty/linenoise:linenoise.BUILD"
    )
