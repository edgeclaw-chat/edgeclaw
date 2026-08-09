load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "libyuarel",
        remote = "https://github.com/jacketizer/libyuarel.git",
        commit = "d4f666065c86cddcfb13bfb89049d06e45117610",
        build_file = "//thirdparty/libyuarel:libyuarel.BUILD",
    )
