load("@rules_cc//cc:defs.bzl", "cc_library")

# ── 工具：定义 + 参数 schema + 6 个实现 + 宿主（toolbox / toolbox_actor）─────
# 工具的"宿主"和工具本身同属一个包：工具经 toolbox 被调用、把结果以消息发出去，
# 两者互相引用；分家会成环，同包则环落在包内（与 kernel 同理由）。
cc_library(
    name = "toolbox",
    srcs = [
        "edc_param_types.c",
        "edc_toolbox.c",
        "edc_toolbox_actor.c",
        "edc_tools.c",
        "edc_tools_crontab.c",
        "edc_tools_device_file.c",
        "edc_tools_device_shell.c",
        "edc_tools_fs.c",
        "edc_tools_shell.c",
        "edc_tools_todo.c",
        "edc_tools_memory.c",
    ],
    hdrs = [
        "edc_param_types.h",
        "edc_toolbox.h",
        "edc_toolbox_actor.h",
        "edc_tools.h",
        "edc_tools_crontab.h",
        "edc_tools_device_file.h",
        "edc_tools_device_shell.h",
        "edc_tools_fs.h",
        "edc_tools_shell.h",
        "edc_tools_todo.h",
        "edc_tools_memory.h",
    ],
    includes = ["."],
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "//src/sandbox:sandbox",
        "//thirdparty/brutezone:brutezone",
        "@ccronexpr//:ccronexpr",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
        "@uthash//:uthash",
    ] + select({
        "//:windows": [],
        "//conditions:default": [
            "//src/device:device",
            "@lwip//:lwip",
            "//thirdparty/lwip:sys_arch",
        ],
    }),
    linkstatic = True,
    visibility = ["//visibility:public"],
)
