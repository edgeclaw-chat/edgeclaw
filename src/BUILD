load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv"],
    "//:macos": ["-lresolv"],
    "//:linux_debug": [],
    "//:debug": [],
    "//:android": [],
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
    name = "edgeclaw",
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
    name = "agent",
    srcs = [
        "edc_context.c",
        "edc_history.c",
        "edc_session.c",
        "edc_turn.c",
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
        "edc_network_async.c",
        "edc_actor.c",
        "edc_channel_actor.c",
        "edc_channel_telegram.c",
        "edc_channel_feishu.c",
        "edc_channel_qq.c",
        "edc_channel_cli.c",
        "edc_llm_actor.c",
        "edc_toolbox_actor.c",
        "edc_memory_actor.c",
        "edc_commands.c",
        "edc_param_types.c",
        "edc_tools_todo.c",
        "edc_markdown_render.c",
        "edc_lwip.c",
        "edc_tools_device_shell.c",
        "edc_tools_device_file.c",
    ],
    hdrs = [
        "edc_context.h",
        "edc_history.h",
        "edc_session.h",
        "edc_turn.h",
        "edc_skills.h",
        "edc_tools.h",
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
        "edc_network_async.h",
        "edc_actor.h",
        "edc_channel_actor.h",
        "edc_channel_telegram.h",
        "edc_channel_feishu.h",
        "edc_channel_qq.h",
        "edc_channel_cli.h",
        "edc_llm_actor.h",
        "edc_toolbox_actor.h",
        "edc_memory_actor.h",
        "edc_commands.h",
        "edc_param_types.h",
        "edc_tools_todo.h",
        "edc_markdown_render.h",
        "edc_lwip.h",
        "edc_tools_device_shell.h",
        "edc_tools_device_file.h",
        "edc_pppos_agent.h",
        "edc_device_agent.h",
        "edc_transport_pppos.h",
    ],
    deps = [
        "@glib//glib:glib",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@linenoise//:linenoise",
        "@mongoose//:mongoose",
        "@ccronexpr//:ccronexpr",
        "@md4c//:md4c",
        ":lark_api_pb",
        ":edc_secret_lib",
        "@lwip//:lwip",
        "//thirdparty/lwip:sys_arch",
        ":pppos_agent_lib",
    ],
    linkopts = select({
        "//:macos": ["-lresolv"],
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
    hdrs = ["edc_secret.h"],
    deps = [
        "@glib//glib:glib",
        "@log.c//:log.c",
        ":mbedtls_config",
        "@mbedtls//:mbedtls",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
