LINUX_LINK_OPTS = [
    "-lrt",
    "-lpthread",
    "-Wl,--no-gc-sections",
]

MACOS_LINK_OPTS = [
    "-lpthread",
    "-lresolv",
]

WINDOWS_LINK_OPTS = [
    "-lws2_32",
    "-lbcrypt",
    "-luserenv",
]

LINK_OPTS = select({
    "//:macos": MACOS_LINK_OPTS,
    "//:linux": LINUX_LINK_OPTS,
    "//:windows": WINDOWS_LINK_OPTS,
    "//conditions:default": [], 
})

cc_test(
    name = "test_containers",
    srcs = ["test_containers.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_channel_actor",
    srcs = ["test_channel_actor.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_llm_actor",
    srcs = ["test_llm_actor.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_memory_actor",
    srcs = ["test_memory_actor.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_lwip_http",
    srcs = ["test_lwip_http.c"],
    deps = [
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_pppos_http",
    srcs = ["test_pppos_http.c"],
    deps = [
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
    ],
    linkopts = LINUX_LINK_OPTS + ["-lutil"],
)

cc_test(
    name = "test_pppos_stress",
    srcs = ["test_pppos_stress.c"],
    deps = [
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
    ],
    linkopts = LINUX_LINK_OPTS + ["-lutil"],
)

cc_test(
    name = "test_pppos_agent",
    srcs = ["test_pppos_agent.c"],
    deps = [
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
        "//src:agent",
    ],
    linkopts = LINUX_LINK_OPTS + ["-lutil"],
)

cc_test(
    name = "test_pppos_agent_stress",
    srcs = ["test_pppos_agent_stress.c"],
    deps = [
        "@cjson//:cjson",
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
        "//src:agent",
    ],
    linkopts = LINUX_LINK_OPTS + ["-lutil"],
)

cc_binary(
    name = "pppos_cli",
    srcs = ["pppos_cli.c"],
    deps = [
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
        "@cjson//:cjson",
        "//src:agent",
    ],
    linkopts = LINUX_LINK_OPTS,
)

cc_test(
    name = "test_strutil",
    srcs = ["test_strutil.c"],
    deps = [
        "//src:edc_string_utils",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_markdown_render",
    srcs = ["test_markdown_render.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_https_client_get",
    srcs = ["test_https_client_get.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_task",
    srcs = ["test_edc_task.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_task_group",
    srcs = ["test_edc_task_group.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_task_stress",
    srcs = ["test_edc_task_stress.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_scheduler",
    srcs = ["test_edc_scheduler.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_session_stress",
    srcs = ["test_edc_session_stress.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_group_level",
    srcs = ["test_group_level.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_shell",
    srcs = ["test_edc_shell.c"],
    deps = [
        "//src:agent",
    ],
    linkopts = LINK_OPTS,
)
