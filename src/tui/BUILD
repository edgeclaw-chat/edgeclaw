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
        "edc_c_bridge.c",
        "edc_chat_widget.cpp",
        "edc_clipboard.cpp",
        "edc_input.cpp",
        "edc_message.cpp",
        "edc_message_list.cpp",
        "edc_theme.cpp",
        "edc_tui_window.cpp",
        "edc_scroll_view.cpp",
        "edc_task_widget.cpp",
        "edc_todo_widget.cpp",
        "markdown.cpp",
    ],

    hdrs = [
        "edc_c_bridge.h",
        "edc_chat_widget.hpp",
        "edc_clipboard.hpp",
        "edc_input.hpp",
        "edc_message.hpp",
        "edc_message_list.hpp",
        "edc_theme.hpp",
        "edc_tui_window.hpp",
        "edc_scroll_view.hpp",
        "edc_style.hpp",
        "edc_task_widget.hpp",
        "edc_text_util.hpp",
        "edc_todo_widget.hpp",
        "markdown.hpp",
    ],
    deps = [
        "//src/agent:agent",
        "//src/shell:shell",
        "@md4c//:md4c",
        "@ftxui//:ftxui",
        "@cjson//:cjson",
    ],
    visibility = ["//visibility:private"],
)

cc_test(
    name = "test_theme",
    srcs = ["test_theme.cpp"],
    deps = [
        ":tui",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "test_scroll_regression",
    srcs = ["test_scroll_regression.cpp"],
    deps = [
        ":tui",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "test_text_wrap",
    srcs = ["test_text_wrap.cpp"],
    deps = [
        ":tui",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "test_edc_input",
    srcs = ["test_edc_input.cpp"],
    deps = [
        ":tui",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "test_working_status",
    srcs = ["test_working_status.cpp"],
    deps = [
        ":tui",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "test_clipboard",
    srcs = ["test_clipboard.cpp"],
    deps = [
        ":tui",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "test_todo_widget",
    srcs = ["test_todo_widget.cpp"],
    deps = [
        ":tui",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
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

cc_test(
    name = "test_message_list",
    srcs = ["test_message_list.cpp"],
    deps = [
        ":tui",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
