load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "lwip",
    hdrs = glob([
        "src/include/**/*.h",
    ]) + select({
        "@//:linux": glob(["contrib/ports/unix/port/include/**/*.h"]),
        "//conditions:default": [],
    }),
    srcs = glob(
        [
            "src/core/*.c",
            "src/core/ipv4/*.c",
            "src/netif/*.c",
            "src/netif/ppp/*.c",
            "src/api/*.c",
        ],
        exclude = [
            "src/api/netdb.c",
            "src/netif/ppp/pppoe.c",
            "src/netif/ppp/pppol2tp.c",
        ],
    ) + select({
        "@//:linux": ["contrib/ports/unix/port/netif/tapif.c"],
        "//conditions:default": [],
    }),
    copts = [
        "-DLWIP_NOASSERT",
    ] + select({
        "@//:linux": ["-DLWIP_UNIX_LINUX"],
        "//conditions:default": [],
    }),
    includes = [
        "src/include",
    ] + select({
        "@//:linux": ["contrib/ports/unix/port/include"],
        "//conditions:default": [],
    }),
    deps = [
        "@//thirdparty/lwip:config",
    ],
    visibility = ["//visibility:public"],
    linkstatic = True,
)
