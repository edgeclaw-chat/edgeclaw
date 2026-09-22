load("@rules_cc//cc:defs.bzl", "cc_library")

# 装配根: 建对象 / 交句柄 / 管生命周期 (只在这里定义 struct edc_app)。
cc_library(
    name = "app",
    srcs = [
            "edc_app.c",
        ],
    hdrs = [
            "edc_app.h",
        ],
    includes = ["."],
    deps = [
            "//src/actor:actor",
            "//src/actor:actors",
            "//src/actor:llm_request",
            "//src/common:common",
            "//src/kernel:kernel",
            "//src/sandbox:sandbox",
        "//src/shell:shell",
            "//src/toolbox:toolbox",
            "@log.c//:log.c",
            "@sds//:sds",
            "@uthash//:uthash",
        ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# 前端的 C 安全面: 从 edc_app.c 拆出的投影层 (消息只读窗口 / 字符串读口 /
# 前端动作)。它只经 edc_app.h 的句柄访问器与命名查询取数 —— 不碰 struct edc_app
# 的字段 (也不碰 session 仓库: 2026-09-22 起那两处投影改成快照字段 + 路径约定),
# 所以装配根不必为它开内部头 (不透明性不退步)。
cc_library(
    name = "api",
    srcs = [
            "edc_api.c",
        ],
    hdrs = [
            "edc_api.h",
        ],
    includes = ["."],
    deps = [
            ":app",
            "//src/actor:actor",
            "//src/actor:actors",
            "//src/common:common",
            "//src/kernel:kernel",
            "@log.c//:log.c",
            "@sds//:sds",
        ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
