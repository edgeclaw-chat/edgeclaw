load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "lwip",
    hdrs = glob([
        "src/include/**/*.h",
        "contrib/ports/unix/port/include/**/*.h",
    ]),
    srcs = glob(
        [
            "src/core/*.c",
            "src/core/ipv4/*.c",
            "src/netif/*.c",
            "src/netif/ppp/*.c",
            "src/api/*.c",
            "contrib/ports/unix/port/netif/tapif.c",
        ],
        exclude = [
            "src/api/netdb.c",
            "src/netif/ppp/pppoe.c",
            "src/netif/ppp/pppol2tp.c",
        ],
    ),
    copts = [
        "-DLWIP_NOASSERT",
        "-DLWIP_UNIX_LINUX",
    ],
    includes = [
        "src/include",
        "contrib/ports/unix/port/include",
    ],
    deps = [
        "@//thirdparty/lwip:config",
    ],
    visibility = ["//visibility:public"],
    linkstatic = True,
)
