cc_library(
    name = "tui",
    srcs = [
        "edc_chat_widget.cpp",
        "edc_tui_window.cpp",
        "edc_message.cpp",
        "edc_scroll_view.cpp",
        "markdown.cpp",
    ],

    hdrs = [
        "edc_chat_widget.hpp",
        "edc_tui_window.hpp",
        "edc_message.hpp",
        "edc_scroll_view.hpp",
        "markdown.hpp",
    ],
    deps = [
        "@md4c//:md4c",
        "@ftxui//:ftxui",
    ],
    visibility = ["//tests:__pkg__"],
)
