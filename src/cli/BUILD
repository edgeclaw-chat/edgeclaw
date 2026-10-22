load("@rules_cc//cc:defs.bzl", "cc_library")

# Non-interactive frontend: --print / --mode json.
#
# A frontend, not an engine: it drives the same app the TUI drives
# (edc_app_new / edc_app_send_user_message / the channel adapter) and has no
# turn logic of its own. Deliberately does NOT depend on //src/tui, so the
# headless path cannot pull in ftxui or any terminal handling.
cc_library(
    name = "headless",
    srcs = [
        "edc_headless.c",
    ],
    hdrs = [
        "edc_headless.h",
    ],
    includes = ["."],
    deps = [
        "//src/app:api",
        "//src/app:app",
        "//src/common:common",
        "@cjson//:cjson",
        "@libuv",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
