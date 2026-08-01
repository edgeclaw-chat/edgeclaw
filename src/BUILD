load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv"],
    "//:macos": ["-lresolv"],
    "//:windows": ["-lws2_32", "-lbcrypt", "-luserenv"],
    "//conditions:default": ["-static", "-Wl,--no-gc-sections"],
})

# ── Top-level utilities ───────────────────────────────────────────────────

cc_library(
    name = "utlist_fix",
    hdrs = ["utlist_fix.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# ── Main binary ───────────────────────────────────────────────────────────

cc_binary(
    name = "edgeclaw.exe",
    srcs = ["main.c"],
    deps = [
        "//src/agent:agent",
        "//src/core:core",
        "//src/device:device",
        "//src/shell:shell",
        ":utlist_fix",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

# ── Backward-compatibility aggregate targets ──────────────────────────────

# Full agent library (agent + shell + core + device) for tests compatibility
cc_library(
    name = "agent",
    deps = [
        "//src/agent:agent",
        "//src/core:core",
        "//src/device:device",
        "//src/shell:shell",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

alias(
    name = "edc_string_utils",
    actual = "//src/core:edc_string_utils",
    visibility = ["//visibility:public"],
)

alias(
    name = "mbedtls_config",
    actual = "//src/core:mbedtls_config",
    visibility = ["//visibility:public"],
)

alias(
    name = "device_agent",
    actual = "//src/device:device_agent",
    visibility = ["//visibility:public"],
)
