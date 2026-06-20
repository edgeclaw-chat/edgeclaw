load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "ccronexpr",
        remote = "https://github.com/exander77/supertinycron",
        commit = "cd1a408cfbcd539a7696d2985ee1cc9b81b5207e",
        build_file = "//thirdparty/ccronexpr:ccronexpr.BUILD",
    )
