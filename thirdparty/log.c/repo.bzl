load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "log.c",
        remote = "https://github.com/rxi/log.c.git",
        commit = "f9ea34994bd58ed342d2245cd4110bb5c6790153",
        build_file = "//thirdparty/log.c:log.c.BUILD",
        # Teaches log.h to declare log_log() with the printf format attribute, so
        # every log_*() call site is format-checked at compile time. Without it a
        # %s handed an int64_t (or any wrong conversion) was accepted silently.
        patches = ["//patches:log-format-attribute.patch"],
        patch_args = ["-p1"],
    )
