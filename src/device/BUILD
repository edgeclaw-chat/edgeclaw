load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv"],
    "//:macos": ["-lresolv"],
    "//:windows": ["-lws2_32", "-lbcrypt", "-luserenv"],
    "//conditions:default": ["-static", "-Wl,--no-gc-sections"],
})

# ── Device agent transport libraries ──────────────────────────────────────

cc_library(
    name = "device_agent_lib",
    srcs = ["edc_device_agent.c"],
    hdrs = ["edc_device_agent.h"],
    includes = ["."],
    deps = ["@sds//:sds"],
    linkstatic = True,
)

cc_library(
    name = "transport_pppos_lib",
    srcs = ["edc_transport_pppos.c"],
    hdrs = [
        "edc_transport_pppos.h",
        "edc_device_agent.h",
    ],
    includes = ["."],
    deps = [
        ":device_agent_lib",
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
        "@cjson//:cjson",
        "@sds//:sds",
    ],
    linkstatic = True,
)

cc_library(
    name = "pppos_agent_lib",
    srcs = ["edc_pppos_agent.c"],
    hdrs = ["edc_pppos_agent.h"],
    includes = ["."],
    deps = [
        ":device_agent_lib",
        ":transport_pppos_lib",
    ],
    linkstatic = True,
)

cc_library(
    name = "transport_socket_lib",
    srcs = ["edc_transport_socket.c"],
    hdrs = [
        "edc_transport_socket.h",
        "edc_device_agent.h",
    ],
    includes = ["."],
    deps = [
        ":device_agent_lib",
        "@cjson//:cjson",
    ],
    linkstatic = True,
)

cc_library(
    name = "socket_agent_lib",
    srcs = ["edc_socket_agent.c"],
    hdrs = ["edc_socket_agent.h"],
    includes = ["."],
    deps = [
        ":device_agent_lib",
        ":transport_socket_lib",
    ],
    linkstatic = True,
)

cc_library(
    name = "device",
    srcs = ["edc_lwip.c"],
    hdrs = ["edc_lwip.h"],
    includes = ["."],
    deps = [
        ":device_agent_lib",
        ":pppos_agent_lib",
        ":socket_agent_lib",
        ":transport_pppos_lib",
        ":transport_socket_lib",
        "//src/core:core",
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
        "@cjson//:cjson",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# ── Standalone device agent binary ────────────────────────────────────────

cc_binary(
    name = "device_agent",
    srcs = ["device_agent.c"],
    deps = [
        ":device_agent_lib",
        ":transport_pppos_lib",
        ":transport_socket_lib",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    visibility = ["//visibility:public"],
)
