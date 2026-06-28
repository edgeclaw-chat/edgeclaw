load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "sds",
        remote = "https://github.com/antirez/sds.git",
        commit = "5347739b1581fcba74fd5cab1fc21d2aef317d71",
        build_file = "//thirdparty/sds:sds.BUILD",
    )
