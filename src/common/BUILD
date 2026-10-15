load("@rules_cc//cc:defs.bzl", "cc_library")

# Top-level utility: utlist 的兼容修正（原 //src:utlist_fix，随 core 一起下沉）
cc_library(
    name = "utlist_fix",
    hdrs = ["utlist_fix.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# ── Individual utility libraries (externally visible) ──────────────────────

# Header-only platform/GLib compatibility shim. Exposed on its own so very
# small binaries (e.g. //src/sandbox:sandbox_helper) can use it without pulling
# the whole //src/common:common library.
cc_library(
    name = "edc_compat",
    hdrs = ["edc_compat.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_error",
    srcs = ["edc_error.c"],
    hdrs = ["edc_error.h"],
    includes = ["."],
    deps = [
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_file_utils",
    srcs = ["edc_file_utils.c"],
    hdrs = ["edc_file_utils.h"],
    includes = ["."],
    deps = [
        "//src/common:edc_compat",
        "//src/common:edc_error",
        "@libuv",
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
        "//src/common:mbedtls_config",
        "//thirdparty/utf8proc",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_datetime_utils",
    srcs = ["edc_datetime_utils.c"],
    hdrs = ["edc_datetime_utils.h"],
    includes = ["."],
    deps = [
        "@sds//:sds",
    ],
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
        "//src/common:edc_error",
        "//src/common:edc_file_utils",
        "//src/common:mbedtls_config",
        "@log.c//:log.c",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# ── Bundled workspace ─────────────────────────────────────────────────────

# Generates a C file that carries the default workspace/ tree as a byte table,
# so the binary is self-contained and edc_init_env() does not need a
# `workspace/` directory on disk (see edc_bundled_workspace.h).
genrule(
    name = "workspace_embedded_c",
    srcs = ["//:workspace_files"],
    outs = ["workspace_embedded.c"],
    cmd = "$(execpath //tools:embed_workspace) --out $@ --prefix workspace --name edc_bundled_workspace $(SRCS)",
    tools = ["//tools:embed_workspace"],
    visibility = ["//visibility:public"],
)

# ── Combined core library (remaining utility modules) ─────────────────────

cc_library(
    name = "common",
    srcs = [
        ":workspace_embedded.c",
        "//:models_catalog_embedded_c",
        "edc_config.c",
        "edc_containers.c",
        "edc_endpoint_probe.c",
        "edc_https_client.c",
        "edc_init_env.c",
        "edc_log.c",
    ],
    hdrs = [
        "edc_bundled_workspace.h",
        "edc_bundled_models_catalog.h",
        "edc_compat.h",
        "edc_config.h",
        "edc_containers.h",
        "edc_endpoint_probe.h",
        "edc_https_client.h",
        "edc_init_env.h",
        "edc_log.h",
        "edc_string.h",
    ],
    includes = ["."],
    deps = [
        "//src/common:edc_datetime_utils",
        "//src/common:edc_error",
        "//src/common:edc_file_utils",
        "//src/common:edc_secret_lib",
        "//src/common:edc_string_utils",
        "//src/common:mbedtls_config",
        "//thirdparty/utf8proc",
        "@cjson//:cjson",
        "@libuv",
        "@libyuarel//:libyuarel",
        "@llhttp//:llhttp",
        "@log.c//:log.c",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
        "@uthash//:uthash",
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
