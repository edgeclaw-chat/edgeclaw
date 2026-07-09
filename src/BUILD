load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv"],
    "//:macos": ["-lresolv"],
    "//:windows": ["-lws2_32", "-lbcrypt", "-luserenv"],
    "//conditions:default": ["-static", "-Wl,--no-gc-sections"],
})

cc_library(
    name = "device_agent_lib",
    srcs = ["edc_device_agent.c"],
    hdrs = ["edc_device_agent.h"],
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
    deps = [
        ":device_agent_lib",
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
        "@cjson//:cjson",
        "@sds//:sds",
    ],
    linkstatic = True,
)

cc_library(
    name = "pppos_agent_lib",
    srcs = ["edc_pppos_agent.c"],
    hdrs = ["edc_pppos_agent.h"],
    deps = [
        ":device_agent_lib",
        ":transport_pppos_lib",
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
    deps = [
        ":device_agent_lib",
        "@cjson//:cjson",
    ],
    linkstatic = True,
)

cc_library(
    name = "socket_agent_lib",
    srcs = ["edc_socket_agent.c"],
    hdrs = ["edc_socket_agent.h"],
    deps = [
        ":device_agent_lib",
        ":transport_socket_lib",
    ],
    linkstatic = True,
)

cc_binary(
    name = "device_agent",
    srcs = ["device_agent.c"],
    deps = [
        ":device_agent_lib",
        ":transport_pppos_lib",
        ":transport_socket_lib",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "edgeclaw.exe",
    srcs = [
        "main.c",
    ],
    deps = [
        ":agent",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "lark_api_pb",
    srcs = ["lark_api.pb.c"],
    hdrs = ["lark_api.pb.h"],
    deps = ["@nanopb//:nanopb"],
)

cc_library(
    name = "edc_error",
    srcs = ["edc_error.c"],
    hdrs = ["edc_error.h"],
    deps = ["@sds//:sds"],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_terminal",
    srcs = ["edc_terminal.c"],
    hdrs = ["edc_terminal.h"],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_file_utils",
    srcs = ["edc_file_utils.c"],
    hdrs = ["edc_file_utils.h"],
    deps = [
        "@libuv",
        ":edc_error",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_string_utils",
    srcs = ["edc_string_utils.c"],
    hdrs = ["edc_string_utils.h"],
    deps = [
        "@sds//:sds",
        ":mbedtls_config",
        "@mbedtls//:mbedtls",
        "//thirdparty/utf8proc",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_datetime_utils",
    srcs = ["edc_datetime_utils.c"],
    hdrs = ["edc_datetime_utils.h"],
    deps = ["@sds//:sds"],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "agent",
    srcs = [
        "edc_context.c",
        "edc_containers.c",
        "edc_history.c",
        "edc_session.c",
        "edc_session_manager.c",
        "edc_turn.c",
        "edc_task.c",
        "edc_cmd_parse.c",
        "edc_task_group.c",
        "edc_scheduler.c",
        "edc_skills.c",
        "edc_tools.c",
        "edc_tools_fs.c",
        "edc_tools_shell.c",
        "edc_tools_crontab.c",
        "edc_sandbox.c",
        "edc_landlock_sandbox.c",
        "edc_bpf_sandbox.c",
        "edc_ns_sandbox.c",
        "edc_agent.c",
        "edc_init_env.c",
        "edc_config.c",
        "edc_log.c",
        "edc_toolbox.c",
        "edc_https_client.c",
        "edc_actor.c",
        "edc_channel_actor.c",
        "edc_channel_cli.c",
        "edc_completion.c",
        "edc_llm_actor.c",
        "edc_toolbox_actor.c",
        "edc_memory_actor.c",
        "edc_commands.c",
        "edc_shell.c",
        "edc_param_types.c",
        "edc_tools_todo.c",
        "edc_markdown_render.c",
        "edc_tools_device_shell.c",
        "edc_tools_device_file.c",
    ] + select({
        "//:windows": [],
        "//conditions:default": ["edc_lwip.c"],
    }),
    hdrs = [
        "edc_compat.h",
        "edc_containers.h",
        "edc_context.h",
        "edc_history.h",
        "edc_session.h",
        "edc_session_manager.h",
        "edc_turn.h",
        "edc_task.h",
        "edc_cmd_parse.h",
        "edc_task_group.h",
        "edc_scheduler.h",
        "edc_skills.h",
        "edc_tools.h",
        "utlist_fix.h",
        "edc_tools_fs.h",
        "edc_tools_shell.h",
        "edc_tools_crontab.h",
        "edc_sandbox.h",
        "edc_landlock_sandbox.h",
        "edc_bpf_sandbox.h",
        "edc_ns_sandbox.h",
        "edc_agent.h",
        "edc_init_env.h",
        "edc_config.h",
        "edc_log.h",
        "edc_toolbox.h",
        "edc_https_client.h",
        "edc_actor.h",
        "edc_channel_actor.h",
        "edc_channel_cli.h",
        "edc_completion.h",
        "edc_llm_actor.h",
        "edc_toolbox_actor.h",
        "edc_memory_actor.h",
        "edc_commands.h",
        "edc_shell.h",
        "edc_param_types.h",
        "edc_tools_todo.h",
        "edc_markdown_render.h",
        "edc_lwip.h",
        "edc_tools_device_shell.h",
        "edc_tools_device_file.h",
        "edc_pppos_agent.h",
        "edc_device_agent.h",
        "edc_transport_pppos.h",
        "edc_string.h",
    ],
    deps = [
        "@libuv",
        ":edc_error",
        ":edc_file_utils",
        ":edc_string_utils",
        ":edc_terminal",
        ":edc_datetime_utils",
        "@uthash//:uthash",
        "@sds//:sds",
        "@cjson//:cjson",
        "@log.c//:log.c",
        ":mbedtls_config",
        "@mbedtls//:mbedtls",
        "@libyuarel//:libyuarel",
        "@llhttp//:llhttp",
        "@ccronexpr//:ccronexpr",
        "@md4c//:md4c",
        ":edc_secret_lib",
        "//thirdparty/brutezone:brutezone",
        "//thirdparty/utf8proc",
    ] + select({
        "//:windows": [],
        "//conditions:default": [
            "@lwip//:lwip",
            "//thirdparty/lwip:sys_arch",
            ":pppos_agent_lib",
        ],
    }),
    linkopts = select({
        "//:macos": ["-lresolv"],
        "//:windows": [],
        "//conditions:default": ["-lpthread"],
    }),
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "mbedtls_config",
    hdrs = ["mbedtls_config.h"],
    includes = ["."],
    defines = ["MBEDTLS_CONFIG_FILE=\\\"mbedtls_config.h\\\""],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_secret_lib",
    srcs = ["edc_secret.c"],
    hdrs = [
        "edc_secret.h",
        "edc_compat.h",
    ],
    deps = [
        ":edc_error",
        ":edc_file_utils",
        ":edc_terminal",
        "@sds//:sds",
        "@log.c//:log.c",
        ":mbedtls_config",
        "@mbedtls//:mbedtls",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
