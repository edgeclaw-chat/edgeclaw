load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")


# ── Kernel sandbox: backends + helper（原 //src/agent 的三个 target 原样搬来）──
cc_library(
    name = "sandbox",
    srcs = [
            "edc_bpf_sandbox.c",
            "edc_landlock_sandbox.c",
            "edc_ns_sandbox.c",
            "edc_sandbox.c",
            "edc_sandbox_helper.c",
            "edc_shell_env.c",
            "sandbox_helper.c",
            ":sandbox_helper_embedded.c",
        ],
    hdrs = [
            "edc_bpf_sandbox.h",
            "edc_landlock_sandbox.h",
            "edc_ns_sandbox.h",
            "edc_sandbox.h",
            "edc_sandbox_helper.h",
            "edc_sandbox_helper_blob.h",
            "edc_shell_env.h",
        ],
    includes = ["."],
    deps = [
        "//src/common:common",
        "//src/common:edc_compat",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)


# ── Kernel sandbox: backends + helper（原 //src/agent 的三个 target 原样搬来）──
cc_library(
    name = "sandbox_backends",
    srcs = ["edc_bpf_sandbox.c", "edc_landlock_sandbox.c", "edc_ns_sandbox.c"],
    hdrs = ["edc_bpf_sandbox.h", "edc_landlock_sandbox.h", "edc_ns_sandbox.h"],
    includes = ["."],
    deps = [
        "//src/common:edc_compat",
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
    deps = [
        "//src/sandbox:sandbox_backends",
    ],
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
