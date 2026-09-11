load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "agent",
    srcs = [
        ":sandbox_helper_embedded.c",
        "edc_actor.c",
        "edc_agent.c",
        "edc_channel_actor.c",
        "edc_channel_adapter.c",
        "edc_context.c",
        "edc_history.c",
        "edc_llm_actor.c",
        "edc_memory_actor.c",
        "edc_ns_sandbox.c",
        "edc_sandbox.c",
        "edc_sandbox_helper.c",
        "edc_scheduler.c",
        "edc_session.c",
        "edc_session_manager.c",
        "edc_skills.c",
        "edc_state_serializer.c",
        "edc_task.c",
        "edc_task_group.c",
        "edc_toolbox.c",
        "edc_toolbox_actor.c",
        "edc_tools.c",
        "edc_tools_crontab.c",
        "edc_tools_device_file.c",
        "edc_tools_device_shell.c",
        "edc_tools_fs.c",
        "edc_tools_shell.c",
        "edc_tools_todo.c",
        "edc_turn.c",
    ],
    hdrs = [
        "edc_actor.h",
        "edc_agent.h",
        "edc_channel_actor.h",
        "edc_channel_adapter.h",
        "edc_context.h",
        "edc_history.h",
        "edc_llm_actor.h",
        "edc_memory_actor.h",
        "edc_ns_sandbox.h",
        "edc_sandbox.h",
        "edc_sandbox_helper.h",
        "edc_sandbox_helper_blob.h",
        "edc_scheduler.h",
        "edc_session.h",
        "edc_session_manager.h",
        "edc_skills.h",
        "edc_state_serializer.h",
        "edc_task.h",
        "edc_task_group.h",
        "edc_toolbox.h",
        "edc_toolbox_actor.h",
        "edc_tools.h",
        "edc_tools_crontab.h",
        "edc_tools_device_file.h",
        "edc_tools_device_shell.h",
        "edc_tools_fs.h",
        "edc_tools_shell.h",
        "edc_tools_todo.h",
        "edc_turn.h",
    ],
    includes = ["."],
    deps = [
        ":sandbox_backends",
        "//src/core:core",
        "//src/shell:completion",
        "//src/shell:shell_api",
        "//src:utlist_fix",
        "@uthash//:uthash",
        "@libyuarel//:libyuarel",
        "@llhttp//:llhttp",
        "@ccronexpr//:ccronexpr",
        "//thirdparty/brutezone:brutezone",
        "//thirdparty/utf8proc",
    ] + select({
        "//:windows": [],
        "//conditions:default": [
            "//src/device:device",
            "@lwip//:lwip",
            "//thirdparty/lwip:sys_arch",
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

# ── Kernel sandbox: backends + helper ─────────────────────────────────────

# Kept separate from //src/agent:agent so the tiny sandbox helper can link just
# these two backends without dragging in libuv / mbedtls / ftxui.
cc_library(
    name = "sandbox_backends",
    srcs = [
        "edc_bpf_sandbox.c",
        "edc_landlock_sandbox.c",
    ],
    hdrs = [
        "edc_bpf_sandbox.h",
        "edc_landlock_sandbox.h",
    ],
    includes = ["."],
    deps = [
        "//src/core:edc_compat",
        "@log.c//:log.c",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# Tiny helper exec'd by the shell tool: applies the kernel sandbox in its own
# fresh single-threaded process and then execs /bin/sh. See sandbox_helper.c.
cc_binary(
    name = "sandbox_helper",
    srcs = ["sandbox_helper.c"],
    deps = [":sandbox_backends"],
    linkstatic = True,
    linkopts = select({
        "//:windows": [],
        "//:macos": [],
        "//conditions:default": ["-static"],
    }),
    visibility = ["//visibility:public"],
)

# Embed the helper executable into the main binary as a byte array, so the
# release stays a single self-contained file (see edc_sandbox_helper.c).
genrule(
    name = "sandbox_helper_embedded_c",
    srcs = [":sandbox_helper"],
    outs = ["sandbox_helper_embedded.c"],
    cmd = "$(execpath //tools:embed_blob) --in $(location :sandbox_helper) --out $@ " +
          "--name edc_sandbox_helper_blob --header edc_sandbox_helper_blob.h",
    tools = ["//tools:embed_blob"],
    visibility = ["//visibility:public"],
)
