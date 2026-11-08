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
            "src/netif/ppp/*.c",
            "src/api/*.c",
        ],
        exclude = [
            "src/api/netdb.c",
            "src/api/sockets.c",
            "src/netif/ppp/pppoe.c",
            "src/netif/ppp/pppol2tp.c",
        ],
    ),
    copts = ["-DLWIP_NOASSERT"],
    includes = ["src/include"],
    deps = [
        "@//thirdparty/lwip:config",
        # lwIP's <lwip/arch.h> gets ssize_t from <unistd.h> when SSIZE_MAX is defined (our
        # sds patch defines it, because otherwise arch.h typedefs ssize_t as `int` on top of
        # the __int64 MSVC build and errors C2371). MSVC has no <unistd.h>, so the stand-in
        # from //src/common:win_compat has to be on this target's include path. Empty on
        # non-Windows platforms.
        "@//src/common:win_compat",
    ],
    visibility = ["//visibility:public"],
    linkstatic = True,
)
