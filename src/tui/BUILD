load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv", "-lc++"],
    "//:macos": ["-lresolv", "-lc++"],
    "//:windows": ["-lws2_32", "-lbcrypt", "-luserenv"],
    # `--define=valgrind=1`: dynamic link so valgrind can intercept malloc.
    "//:valgrind": ["-lstdc++"],
    "//conditions:default": ["-static", "-Wl,--no-gc-sections", "-lstdc++"],
})

cc_library(
    name = "tui",
    srcs = [
        "edc_chat_transcript.cpp",
        "edc_chat_widget.cpp",
        "edc_clipboard.cpp",
        "edc_input.cpp",
        "edc_message.cpp",
        "edc_message_list.cpp",
        "edc_theme.cpp",
        "edc_tui_window.cpp",
        "edc_scroll_view.cpp",
        "edc_sidebar.cpp",
        "edc_task_snapshot.cpp",
        "edc_task_widget.cpp",
        "edc_todo_widget.cpp",
        "markdown.cpp",
    ],

    hdrs = [
        "edc_chat_transcript.hpp",
        "edc_chat_widget.hpp",
        "edc_clipboard.hpp",
        "edc_input.hpp",
        "edc_message.hpp",
        "edc_message_list.hpp",
        "edc_theme.hpp",
        "edc_tui_window.hpp",
        "edc_scroll_view.hpp",
        "edc_sidebar.hpp",
        "edc_style.hpp",
        "edc_task_snapshot.hpp",
        "edc_task_widget.hpp",
        "edc_text_util.hpp",
        "edc_todo_widget.hpp",
        "markdown.hpp",
    ],
    includes = ["."],
    deps = [
        "//src/app:app",
        "//src/common:common",
        "@cjson//:cjson",
        "@ftxui//:dom",
        "@ftxui//:ftxui",
        "@libuv",
        "@log.c//:log.c",
        "@md4c//:md4c",
    ],
    visibility = ["//tests:__pkg__"],
)





















cc_binary(
    name = "edgeclaw_tui",
    srcs = [
        "main.cpp",
    ],
    deps = [
        "//src/actor:actors",
        "//src/app:app",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "//src/shell:shell",
        "//src/tui:tui",
        "@ftxui//:dom",
        "@libuv",
        "@log.c//:log.c",
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
