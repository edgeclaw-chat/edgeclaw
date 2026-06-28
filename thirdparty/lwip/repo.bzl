load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "lwip",
        remote = "https://github.com/lwip-tcpip/lwip.git",
        commit = "77dcd25a72509eb83f72b033d219b1d40cd8eb95",
        build_file = "//thirdparty/lwip:lwip.BUILD",
    )
