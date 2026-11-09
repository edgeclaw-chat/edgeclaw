load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "config",
    hdrs = select({
        "@platforms//os:windows": [
            "lwipopts.h",
            "arch/win32/cc.h",
            "arch/win32/sys_arch.h",
        ],
        "//conditions:default": [
            "lwipopts.h",
            "arch/cc.h",
            "arch/sys_arch.h",
        ],
    }),
    includes = ["."],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "sys_arch",
    srcs = select({
        "@platforms//os:windows": ["arch/win32/sys_arch.c"],
        "//conditions:default": ["arch/sys_arch.c"],
    }),
    includes = ["."],
    deps = [
        ":config",
        "@lwip//:lwip",
    ],
    # Same reason as @lwip//:lwip above: this is the ppp/pty transport's threading layer, and
    # the Win32 branch here is stale against the pinned lwIP (it does `sem->sem` on a type this
    # release redefines). No Windows artifact links it.
    target_compatible_with = select({
        "@platforms//os:windows": ["@platforms//os:none"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    linkstatic = True,
    alwayslink = True,
)
