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


# ── Shell 解析原语 (leaf: 只依赖 core，agent 可直接依赖) ──────
#
# 刻意不依赖 //src/agent:agent：agent 侧需要 cmd_parse（/task 参数解析）
# 与 shell_vars（$VAR 展开），而下面的 shell 执行器又依赖 agent。把这
# 两个 leaf 抽到本 target 后，依赖方向是 agent → shell_parse 单向，原来的
# agent ↔ shell 环被断开（`bazel query 'somepath(//src/agent:agent,//src/shell:shell)'`
# 为空）。不要往这里加任何依赖 agent 的源文件。
cc_library(
    name = "shell_parse",
    srcs = [
        "edc_cmd_parse.c",
        "edc_shell_vars.c",
    ],
    hdrs = [
        "edc_cmd_parse.h",
        "edc_shell_vars.h",
    ],
    includes = ["."],
    deps = [
        "//src/core:core",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# ── Shell 实现 (依赖 agent，仅供顶层二进制使用) ─────────────

cc_library(
    name = "shell",
    srcs = [
        "edc_commands.c",
        "edc_shell.c",
        "edc_shell_ast.c",
        "edc_shell_parse.c",
        "edc_shell_token.c",
    ],
    hdrs = [
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
        ":shell_parse",
        "//src/agent:agent",
        "//src/core:core",
        "//src:utlist_fix",
        "@uthash//:uthash",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
