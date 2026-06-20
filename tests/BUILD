LINUX_LINK_OPTS = [
    "-lrt",
    "-lpthread",
    "-Wl,--no-gc-sections",
]

MACOS_LINK_OPTS = [
    "-lpthread",
    "-lresolv",
]

LINK_OPTS = select({
    "//:macos": MACOS_LINK_OPTS,
    "//:linux": LINUX_LINK_OPTS,
    "//conditions:default": [], 
})

cc_test(
    name = "test_mongoose_https_client",
    srcs = ["test_mongoose_https_client.c"],
    deps = [
        "@mongoose//:mongoose",
        "@glib//glib:glib",
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_mongoose_resolve",
    srcs = ["test_mongoose_resolve.c"],
    deps = [
        "@mongoose//:mongoose",
        "@glib//glib:glib",
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_channel_actor",
    srcs = ["test_channel_actor.c"],
    deps = [
        "@mongoose//:mongoose",
        "@glib//glib:glib",
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_llm_actor",
    srcs = ["test_llm_actor.c"],
    deps = [
        "@mongoose//:mongoose",
        "@glib//glib:glib",
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_memory_actor",
    srcs = ["test_memory_actor.c"],
    deps = [
        "@mongoose//:mongoose",
        "@glib//glib:glib",
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_exec_cmdline_async",
    srcs = ["test_exec_cmdline_async.c"],
    deps = [
        "@glib//glib:glib",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_markdown_render",
    srcs = ["test_markdown_render.c"],
    deps = [
        "@glib//glib:glib",
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)
