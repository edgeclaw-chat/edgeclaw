load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv", "-lc++"],
    "//:macos": ["-lresolv", "-lc++"],
    "//:windows": ["-lws2_32", "-lbcrypt", "-luserenv"],
    "//conditions:default": ["-static", "-Wl,--no-gc-sections"],
})

cc_library(
    name = "tui",
    srcs = [
        "edc_c_bridge.c",
        "edc_chat_widget.cpp",
        "edc_tui_window.cpp",
        "edc_message.cpp",
        "edc_scroll_view.cpp",
        "markdown.cpp",
    ],

    hdrs = [
        "edc_c_bridge.h",
        "edc_chat_widget.hpp",
        "edc_tui_window.hpp",
        "edc_message.hpp",
        "edc_scroll_view.hpp",
        "markdown.hpp",
    ],
    deps = [
        "//src/agent:agent",
        "@md4c//:md4c",
        "@ftxui//:ftxui",
    ],
    visibility = ["//visibility:private"],
)

cc_binary(
    name = "edgeclaw_tui",
    srcs = [
        "main.cpp",
    ],
    deps = [
        "//src/agent:agent",
        "//src/core:core",
        "//src/shell:shell",
        "//src:utlist_fix",
        ":tui",
    ] + select({
        "//:windows": [],
        "//conditions:default": ["//src/device:device"],
    }),
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
