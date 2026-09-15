load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "assembly",
    srcs = [
            "edc_cli_history.c",
            "edc_runtime.c",
        ],
    hdrs = [
            "edc_cli_history.h",
            "edc_runtime.h",
        ],
    includes = ["."],
    deps = [
            "//src/actor:actor",
            "//src/actors:actors",
            "//src/common:common",
            "//src/common:utlist_fix",
            "//src/kernel:kernel",
            "//src/sandbox:sandbox",
            "//src/toolbox:toolbox",
            "@log.c//:log.c",
            "@sds//:sds",
            "@uthash//:uthash",
        ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
