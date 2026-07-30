cc_library(
    name = "message",
    srcs = [
        "edc_message.cpp",
    ],
    hdrs = [
        "edc_message.hpp",
    ],
    deps = [
        "@ftxui//:ftxui",
    ],
    visibility = ["//tests:__pkg__"],
)

cc_library(
    name = "tui",
    srcs = [
        "edc_chat_widget.cpp",
    ],

    hdrs = [
        "edc_chat_widget.hpp",
    ],
    deps = [
        ":message",
    ],
    visibility = ["//tests:__pkg__"],
)
