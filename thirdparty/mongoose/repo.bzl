load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "mongoose",
        remote = "https://github.com/cesanta/mongoose.git",
        commit = "1bb85799ce61b5c4ca43df7e74a6759e02c60e03",
        build_file = "//thirdparty/mongoose:mongoose.BUILD",
    )
