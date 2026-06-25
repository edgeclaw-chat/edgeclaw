load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "lwip",
    hdrs = glob([
        "src/include/**/*.h",
    ]),
    srcs = glob(
        [
            "src/core/*.c",
            "src/core/ipv4/*.c",
            "src/netif/*.c",
            "src/api/*.c",
        ],
        exclude = [
            "src/api/netdb.c",
        ],
    ),
    copts = [
        "-DLWIP_NOASSERT",
    ],
    includes = ["src/include"],
    deps = [
        "@//thirdparty/lwip:config",
    ],
    visibility = ["//visibility:public"],
    linkstatic = True,
)
