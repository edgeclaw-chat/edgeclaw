cc_library(
    name = "tui",
    srcs = [
        "edc_chat_widget.cpp",
        "edc_message.cpp",
        "markdown.cpp",
    ],

    hdrs = [
        "edc_chat_widget.hpp",
        "edc_message.hpp",
        "markdown.hpp",
    ],
    deps = [
        "@md4c//:md4c",
        "@ftxui//:ftxui",
    ],
    visibility = ["//tests:__pkg__"],
)
