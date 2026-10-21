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

# ── 宿主工具检查（flex / bison 不 hermetic；docs/input-pipeline.zh.md §11） ──
#
# 生成器调的是构建机上的 flex / bison，缺了或太老时 Bazel 只会把工具自己的报错
# 原样抛出来，看不出缺什么：macOS 自带的 bison 是 2006 年的 2.3（Xcode 的 shim
# 指向同一个），它没有 -W 选项，也不认文法里的 %define api.value.type /
# %define parse.error，只报一句 `invalid option -- W`；Git for Windows 干脆不带
# flex/bison，报的是 `flex: command not found`。先查一遍，把"装哪个"直接说出来。
#
# 只用管道和单引号：genrule 的 cmd 会被 Bazel 先做一遍 make 变量展开，`$(` / `$$(`
# 写在这里会被提前替掉（Windows 上直接变成 shell 语法错误），所以这段不取变量。
_HOST_TOOLS_CHECK = (
    "command -v flex >/dev/null 2>&1 || " +
    "{ echo 'error: flex not found on PATH; install flex and m4 " +
    "(macOS: brew install flex; Windows: MSYS2 pacman -S flex m4)' >&2; exit 1; }; " +
    "command -v bison >/dev/null 2>&1 || " +
    "{ echo 'error: bison not found on PATH; GNU Bison 3.0+ is required " +
    "(macOS: brew install bison; Windows: MSYS2 pacman -S bison)' >&2; exit 1; }; " +
    "bison --version 2>/dev/null | head -n1 | grep -qE 'Bison\\) ([3-9]|[1-9][0-9])\\.' || " +
    "{ echo 'error: the bison on PATH is too old: GNU Bison 3.0+ is required for " +
    "-Werror=conflicts-* and %define api.value.type. This is what was found:' >&2; " +
    "bison --version 2>/dev/null | head -n1 >&2; " +
    "echo 'error: macOS: brew install bison, then put that keg on PATH first " +
    "(/opt/homebrew/opt/bison/bin on Apple silicon, /usr/local/opt/bison/bin on Intel); " +
    "Windows: MSYS2 pacman -S bison' >&2; " +
    "exit 1; }; "
)

# ── 新输入流水线的词法器（flex 生成；docs/input-pipeline.zh.md §3） ──────
#
# 与老的 edc_shell_token.c 并存：第①步只落地 + 单测，尚未接进生产。
# 生成物是 bazel 的 outs，不签入仓库（构建机需要 flex；见 §11 的代价说明）。

genrule(
    name = "lex_c",
    srcs = ["edc_lex.l"],
    outs = ["edc_lex.c"],
    cmd = _HOST_TOOLS_CHECK + "flex -o $@ $(location edc_lex.l)",
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
    cmd = _HOST_TOOLS_CHECK + "bison --defines=$(location edc_parse.tab.h) -o $(location edc_parse.tab.c) " +
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
