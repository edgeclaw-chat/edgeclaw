load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "llhttp",
        remote = "https://github.com/nodejs/llhttp.git",
        commit = "751e5b44dc07a9932d244cb06210cfdcae951115",
        build_file = "//thirdparty/llhttp:llhttp.BUILD",
    )
