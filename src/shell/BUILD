load("@rules_cc//cc:defs.bzl", "cc_library")

# ── Shell 解析原语 (leaf: 只依赖 common，app 可直接依赖) ──────
#
# 刻意不依赖 //src/app:app：app 侧需要 cmd_parse（/task 参数解析）
# 与 shell_vars（{{…}} 任务引用展开），而下面的 shell 执行器又依赖 app。把这
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
        "edc_interp.c",
        "edc_local_shell.c",
        "edc_shell.c",
    ],
    hdrs = [
        "edc_commands.h",
        "edc_interp.h",
        "edc_local_shell.h",
        "edc_shell.h",
    ],
    includes = [
        ".",
    ],
    deps = [
        ":parse",
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

# ── 新输入流水线的词法器（flex 生成；docs/input-pipeline.zh.md §3） ──────
#
# 与老的 edc_shell_token.c 并存：第①步只落地 + 单测，尚未接进生产。
# 生成物是 bazel 的 outs，不签入仓库（构建机需要 flex；见 §11 的代价说明）。

genrule(
    name = "lex_c",
    srcs = ["edc_lex.l"],
    outs = ["edc_lex.c"],
    cmd = "flex -o $@ $(location edc_lex.l)",
)

cc_library(
    name = "lex",
    srcs = ["edc_lex.c"],
    hdrs = ["edc_lex.h"],
    includes = ["."],
    deps = [
        ":shell_parse",
        "//src/common:common",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# ── 新输入流水线的文法层（bison 生成；docs/input-pipeline.zh.md §4） ──────
#
# -Werror=conflicts-* 是关键：任何人往文法里加一条有歧义的规则，构建直接失败。
genrule(
    name = "parse_c",
    srcs = ["edc_parse.y"],
    outs = [
        "edc_parse.tab.c",
        "edc_parse.tab.h",
    ],
    cmd = "bison --defines=$(location edc_parse.tab.h) -o $(location edc_parse.tab.c) " +
          "-Werror=conflicts-sr -Werror=conflicts-rr $(location edc_parse.y)",
)

cc_library(
    name = "parse",
    srcs = ["edc_parse.tab.c"],
    hdrs = [
        "edc_ast.h",
        "edc_parse.h",
        "edc_parse.tab.h", # bison 生成：要在 hdrs 里，沙箱才会把它放进来
    ],
    includes = ["."],
    deps = [
        ":lex",
        "//src/common:common",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
