load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "actor",
    srcs = [
            "edc_actor.c",
        ],
    hdrs = [
            "edc_actor.h",
        ],
    includes = ["."],
    deps = [
            "//src/common:common",
            "@sds//:sds",
        ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
