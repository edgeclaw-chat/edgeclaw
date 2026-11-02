load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

# The headless frontend: the TUI's input box, without the screen.
#
# Deliberately does NOT depend on //src/tui -- not even transitively. That is what makes
# `bazel query 'deps(//src/cli:edgeclaw_cli)'` show no ftxui: this binary cannot take over
# a terminal even by accident, and it is much smaller than the interactive one.
cc_library(
    name = "cli",
    srcs = [
        "edc_cli.c",
    ],
    hdrs = [
        "edc_cli.h",
    ],
    includes = ["."],
    deps = [
        "//src/app:api",
        "//src/app:app",
        "//src/common:common",
        "//src/common:version",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@libuv",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv"],
    "//:macos": ["-lresolv"],
    "//:windows": ["-lws2_32", "-lbcrypt", "-luserenv"],
    # `--define=valgrind=1`: dynamic link so valgrind can intercept malloc.
    "//:valgrind": [],
    # `--config=asan` sets //:sanitize: ASAN cannot link statically.
    "//:sanitize": [],
    "//conditions:default": ["-static", "-Wl,--no-gc-sections"],
})

cc_binary(
    name = "edgeclaw_cli",
    srcs = ["main_cli.c"],
    deps = [
        ":cli",
        "//src/common:common",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    visibility = ["//visibility:public"],
)
