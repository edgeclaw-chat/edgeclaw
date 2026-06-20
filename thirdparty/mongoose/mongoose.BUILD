load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "mongoose",
    hdrs = ["mongoose.h"],
    srcs = ["mongoose.c"],
    copts = [
        "-DMG_ENABLE_LINES",
        "-DMG_TLS=MG_TLS_MBED",
        "-DMG_IO_SIZE=32768",
        "-DMG_ENABLE_POLL=1",
    ],
    deps = [
        "@mbedtls//:mbedtls"
    ],
    visibility = ["//visibility:public"],
    linkstatic = True,
)
