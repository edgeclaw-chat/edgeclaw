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
        "edc_mention.c",
        "edc_shell_vars.c",
    ],
    hdrs = [
        "edc_cmd_parse.h",
        "edc_mention.h",
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
        "edc_pass_mentions.c",
        "edc_passes.c",
        "edc_text_passes.c",
        "edc_shell.c",
    ],
    hdrs = [
        "edc_commands.h",
        "edc_interp.h",
        "edc_local_shell.h",
        "edc_passes.h",
        "edc_text_passes.h",
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
        "//src/sandbox:sandbox",
        "//src/shell:shell_parse",
        "@log.c//:log.c",
        "@sds//:sds",
        "@uthash//:uthash",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# ── 新输入流水线的词法器 ──────────────────────────────────────────────
#
# 与老的 edc_shell_token.c 并存：第①步只落地 + 单测，尚未接进生产。
#
# edc_lex.c 是**签入仓库**的生成物，构建时直接编译它，不再调 flex：
# 生成器曾经是每次构建都跑的 genrule，于是 flex/bison 变成构建机的硬性前置——
# macOS 自带 2006 年的 bison（不认 %define api.value.type），Git for Windows 干脆
# 不带这两个工具，每个 CI job 都要装一遍（macOS 还得手动把 keg 塞进 PATH）。
# 症状是"什么都没改，换台机器就编不过"。
#
# 现在的风险转移到"改了 .l/.y 忘记重新生成"，由 tools/regen_grammar.sh --check
# 在 CI 里兜住：它重新生成并比对，不一致就失败。**改文法后必须跑
# `tools/regen_grammar.sh` 并提交生成物。**
#
# 下面的 genrule 保留但标了 manual，只作为"如何生成"的可执行说明（regen 脚本是
# 权威入口，它会钉住工具版本）。它不在任何默认构建路径上。

genrule(
    name = "lex_c",
    srcs = ["edc_lex.l"],
    outs = ["edc_lex.regen.c"],
    cmd = "flex -o $@ $(location edc_lex.l)",
    tags = ["manual"],
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

# ── 新输入流水线的文法层 ──────────────────────────────────────────────
#
# edc_parse.tab.c / .h 都是签入仓库的生成物，理由同上面 lex_c：构建不再依赖 bison。
# -Werror=conflicts-* 仍然强制，只是时点移到了生成（regen / --check）时——见
# tools/regen_grammar.sh。
genrule(
    name = "parse_c",
    srcs = ["edc_parse.y"],
    outs = [
        "edc_parse.regen.tab.c",
        "edc_parse.regen.tab.h",
    ],
    cmd = "bison --defines=$(location edc_parse.regen.tab.h) " +
          "-o $(location edc_parse.regen.tab.c) " +
          "-Werror=conflicts-sr -Werror=conflicts-rr $(location edc_parse.y)",
    tags = ["manual"],
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
