load("@rules_cc//cc:defs.bzl", "cc_library")

# ── Individual utility libraries (externally visible) ──────────────────────

cc_library(
    name = "edc_error",
    srcs = ["edc_error.c"],
    hdrs = ["edc_error.h"],
    includes = ["."],
    deps = ["@sds//:sds"],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_terminal",
    srcs = ["edc_terminal.c"],
    hdrs = ["edc_terminal.h"],
    includes = ["."],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_file_utils",
    srcs = ["edc_file_utils.c"],
    hdrs = ["edc_file_utils.h"],
    includes = ["."],
    deps = [
        "@libuv",
        ":edc_error",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_string_utils",
    srcs = ["edc_string_utils.c"],
    hdrs = ["edc_string_utils.h"],
    includes = ["."],
    deps = [
        "@sds//:sds",
        ":mbedtls_config",
        "@mbedtls//:mbedtls",
        "//thirdparty/utf8proc",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_datetime_utils",
    srcs = ["edc_datetime_utils.c"],
    hdrs = ["edc_datetime_utils.h"],
    includes = ["."],
    deps = ["@sds//:sds"],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "mbedtls_config",
    hdrs = ["mbedtls_config.h"],
    includes = ["."],
    defines = ["MBEDTLS_CONFIG_FILE=\\\"mbedtls_config.h\\\""],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_secret_lib",
    srcs = ["edc_secret.c"],
    hdrs = [
        "edc_secret.h",
        "edc_compat.h",
    ],
    includes = ["."],
    deps = [
        ":edc_error",
        ":edc_file_utils",
        ":edc_terminal",
        "@sds//:sds",
        "@log.c//:log.c",
        ":mbedtls_config",
        "@mbedtls//:mbedtls",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# ── Combined core library (remaining utility modules) ─────────────────────

cc_library(
    name = "core",
    srcs = [
        "edc_config.c",
        "edc_containers.c",
        "edc_https_client.c",
        "edc_init_env.c",
        "edc_log.c",
        "edc_markdown_render.c",
        "edc_param_types.c",
    ],
    hdrs = [
        "edc_compat.h",
        "edc_config.h",
        "edc_containers.h",
        "edc_https_client.h",
        "edc_init_env.h",
        "edc_log.h",
        "edc_markdown_render.h",
        "edc_param_types.h",
        "edc_string.h",
        "edc_task_types.h",
    ],
    includes = ["."],
    deps = [
        ":edc_datetime_utils",
        ":edc_error",
        ":edc_file_utils",
        ":edc_secret_lib",
        ":edc_string_utils",
        ":edc_terminal",
        ":mbedtls_config",
        "@cjson//:cjson",
        "@libuv",
        "@libyuarel//:libyuarel",
        "@llhttp//:llhttp",
        "@log.c//:log.c",
        "@mbedtls//:mbedtls",
        "@md4c//:md4c",
        "@sds//:sds",
        "@uthash//:uthash",
        "//thirdparty/utf8proc",
    ],
    # Debug builds keep trace/debug logging; normal builds clamp to INFO
    # (see edc_log_init). Same signal as the rest of the tree.
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
    linkstatic = True,
    visibility = ["//visibility:public"],
)
