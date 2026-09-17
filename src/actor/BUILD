load("@rules_cc//cc:defs.bzl", "cc_library")

# actor 基座：邮箱 + 消息种类。本仓零依赖（只依赖 common / sds）。
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

# 具体 actor（channel 入/出站翻译缝 · llm 宿主）。
# 唯一不能并进 :actor 的东西是它对 kernel 的依赖（`edc_scheduler.h` 的 `struct edc_sched_input`），
# 而 kernel 自己依赖 :actor —— 合成一个 target 就是 bazel 环（依赖粒度是 target 不是 package）。
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
            ":actor",
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

# LLM 请求装配器：与 llm_actor 同概念的另一半（谁定请求体 / 谁发请求）。
# 不含 toolbox —— 工具定义由装配根当参数传入（见 edc_llm_request.h）。
cc_library(
    name = "llm_request",
    srcs = [
            "edc_llm_request.c",
        ],
    hdrs = [
            "edc_llm_request.h",
        ],
    includes = ["."],
    deps = [
            ":actor",
            ":actors",
            "//src/common:common",
            "//src/common:utlist_fix",
            "//src/kernel:kernel",
            "//src/shell:shell_parse",
            "@cjson//:cjson",
            "@log.c//:log.c",
            "@sds//:sds",
        ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
