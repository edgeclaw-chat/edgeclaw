load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "config",
    hdrs = [
        "lwipopts.h",
        "arch/cc.h",
        "arch/sys_arch.h",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "sys_arch",
    srcs = ["arch/sys_arch.c"],
    includes = ["."],
    deps = [
        ":config",
        "@lwip//:lwip",
    ],
    linkopts = ["-lpthread"],
    visibility = ["//visibility:public"],
    linkstatic = True,
)
