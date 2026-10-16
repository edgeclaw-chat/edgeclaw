load("@rules_cc//cc:defs.bzl", "cc_library")

# ── Shell 解析原语 (leaf: 只依赖 common，app 可直接依赖) ──────
#
# 刻意不依赖 //src/app:app：app 侧需要 cmd_parse（/task 参数解析）
# 与 shell_vars（$VAR 展开），而下面的 shell 执行器又依赖 app。把这
# 两个 leaf 抽到本 target 后，依赖方向是 app → shell_parse 单向，原来的
# app ↔ shell 环被断开（`bazel query 'somepath(//src/app:app,//src/shell:shell)'`
# 为空）。不要往这里加任何依赖 app 的源文件。
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
        "//src/common:common",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# ── Shell 实现 (依赖 app，仅供顶层二进制使用) ─────────────

cc_library(
    name = "shell",
    srcs = [
        "edc_commands.c",
        "edc_local_shell.c",
        "edc_shell.c",
        "edc_shell_ast.c",
        "edc_shell_parse.c",
        "edc_shell_token.c",
    ],
    hdrs = [
        "edc_commands.h",
        "edc_local_shell.h",
        "edc_shell.h",
        "edc_shell_ast.h",
        "edc_shell_token.h",
    ],
    includes = [
        ".",
    ],
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "//src/shell:shell_parse",
        "@log.c//:log.c",
        "@sds//:sds",
        "@uthash//:uthash",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
