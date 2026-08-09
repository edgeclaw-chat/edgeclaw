load("@rules_cc//cc:defs.bzl", "cc_library")

# ── Completion (纯独立，不依赖 agent) ────────────────────────

cc_library(
    name = "completion",
    srcs = ["edc_completion.c"],
    hdrs = ["edc_completion.h"],
    includes = ["."],
    deps = [
        "//src/core:core",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)


cc_library(
    name = "shell_api",
    srcs = ["edc_shell_vars.c"],
    hdrs = [
        "edc_commands.h",
        "edc_completion.h",
        "edc_shell.h",
    ],
    includes = ["."],
    deps = [
        "//src/core:core",
    ],
    visibility = ["//visibility:public"],
)

# ── Shell 实现 (依赖 agent，仅供顶层二进制使用) ─────────────

cc_library(
    name = "shell",
    srcs = [
        "edc_cmd_parse.c",
        "edc_commands.c",
        "edc_shell.c",
        "edc_shell_ast.c",
        "edc_shell_parse.c",
        "edc_shell_token.c",
    ],
    hdrs = [
        "edc_cmd_parse.h",
        "edc_commands.h",
        "edc_completion.h",
        "edc_shell.h",
        "edc_shell_ast.h",
        "edc_shell_token.h",
    ],
    includes = [
        ".",
    ],
    deps = [
        ":completion",
        ":shell_api",
        "//src/agent:agent",
        "//src/core:core",
        "//src:utlist_fix",
        "@uthash//:uthash",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
