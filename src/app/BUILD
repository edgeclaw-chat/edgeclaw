load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "app",
    srcs = [
            "edc_app.c",
        ],
    hdrs = [
            "edc_app.h",
        ],
    includes = ["."],
    deps = [
            "//src/actor:actor",
            "//src/actor:actors",
            "//src/actor:llm_request",
            "//src/common:common",
            "//src/kernel:kernel",
            "//src/sandbox:sandbox",
        "//src/shell:shell",
            "//src/toolbox:toolbox",
            "@log.c//:log.c",
            "@sds//:sds",
            "@uthash//:uthash",
        ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
