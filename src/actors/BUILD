load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "actors",
    srcs = [
            "edc_channel_actor.c",
            "edc_llm_actor.c",
        ],
    hdrs = [
            "edc_channel_actor.h",
            "edc_llm_actor.h",
        ],
    includes = ["."],
    deps = [
            "//src/actor:actor",
            "//src/common:common",
            "//src/kernel:kernel",
            "@cjson//:cjson",
            "@log.c//:log.c",
            "@sds//:sds",
            "@uthash//:uthash",
        ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
