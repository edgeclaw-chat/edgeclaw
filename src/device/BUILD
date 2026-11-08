load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv"],
    "//:macos": ["-lresolv"],
    # `-static` for the same reason as //src/tui: the shipped Windows artifact must not
    # depend on libgcc/libwinpthread DLLs that the tarball does not carry. See the
    # longer note in src/tui/BUILD.
    "//:windows": ["ws2_32.lib", "bcrypt.lib", "userenv.lib"],
    # `--config=asan` sets //:sanitize: ASAN cannot link statically.
    "//:sanitize": [],
    "//conditions:default": ["-static", "-Wl,--no-gc-sections"],
})

# ── Device agent transport libraries ──────────────────────────────────────

cc_library(
    name = "device_agent_lib",
    srcs = ["edc_device_agent.c"],
    hdrs = ["edc_device_agent.h"],
    includes = ["."],
    deps = [
        "@sds//:sds",
    ],
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
        "//src/device:device_agent_lib",
        "//thirdparty/lwip:sys_arch",
        "@cjson//:cjson",
        "@lwip//:lwip",
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
        "//src/device:device_agent_lib",
        "//src/device:transport_pppos_lib",
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
        "//src/device:device_agent_lib",
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
        "//src/device:device_agent_lib",
        "//src/device:transport_socket_lib",
    ],
    linkstatic = True,
)

cc_library(
    name = "device",
    srcs = ["edc_lwip.c"],
    hdrs = ["edc_lwip.h"],
    includes = ["."],
    deps = [
        "//src/common:common",
        "//src/device:device_agent_lib",
        "//src/device:pppos_agent_lib",
        "//src/device:socket_agent_lib",
        "//src/device:transport_pppos_lib",
        "//src/device:transport_socket_lib",
        "//thirdparty/lwip:sys_arch",
        "@cjson//:cjson",
        "@lwip//:lwip",
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
        "//src/device:device_agent_lib",
        "//src/device:transport_pppos_lib",
        "//src/device:transport_socket_lib",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    visibility = ["//visibility:public"],
)
