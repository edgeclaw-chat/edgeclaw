load("@rules_cc//cc:defs.bzl", "cc_library")

# Top-level utility: utlist 的兼容修正（原 //src:utlist_fix，随 core 一起下沉）
cc_library(
    name = "utlist_fix",
    hdrs = ["utlist_fix.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# ── Individual utility libraries (externally visible) ──────────────────────

# The version facts, derived at build time from `git describe` (tools/gen_version.py).
# `local = 1` so the action can read .git, which the sandbox does not expose; the rule is
# cheap and its output changes only when the commit or the dirty state does.
genrule(
    name = "version_header",
    # //:BUILD is declared only so its location can be passed to the script: resolving it
    # is how the script finds the checkout that owns .git (see gen_version.py).
    srcs = ["//:BUILD"],
    outs = ["edc_version_gen.h"],
    cmd = "$(execpath //tools:gen_version) --out $@ --repo $(location //:BUILD)",
    cmd_bat = "$(execpath //tools:gen_version) --out $@ --repo $(location //:BUILD)",
    tools = ["//tools:gen_version"],
    # stamp = 1 makes this action ALWAYS re-run, and that is load-bearing rather than
    # cosmetic. The facts come from git, which Bazel cannot see: with the default inputs the
    # only thing the action depends on is BUILD's content, so a header generated while the
    # tree was dirty survived every later build -- `--version` kept reporting "-dirty" on a
    # clean checkout, and would have kept reporting an older commit after a commit that did
    # not touch BUILD. Depending on the volatile status file is what makes the action
    # out-of-date each build, so the string always describes the tree that was built.
    stamp = 1,
    local = 1,
    tags = ["no-sandbox"],
    visibility = ["//visibility:public"],
)

# A tiny target of its own so a binary that only wants `--version` does not have to pull
# the whole //src/common:common tree (edc_config, HTTPS, the embedded workspace...).
cc_library(
    name = "version",
    srcs = ["edc_version.c"],
    hdrs = ["edc_version.h", ":version_header"],
    includes = ["."],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# The `--update-check` command. Its own target rather than part of :common because it needs
# :version (which :common deliberately does not depend on), and a separate one keeps the
# dependency direction readable: the diagnostic sits on top of the check, not beside it.
# Runs the installer. Separate from :common because it needs :version's sibling -- the argv
# it spawns comes from edc_upgrade, and it is only ever used by the app, never by a core path.
cc_library(
    name = "update_run",
    srcs = ["edc_upgrade_run.c"],
    hdrs = ["edc_upgrade_run.h"],
    includes = ["."],
    deps = [
        ":common",
        "@libuv",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "update_cli",
    srcs = ["edc_update_cli.c"],
    hdrs = ["edc_update_cli.h"],
    includes = ["."],
    deps = [
        ":common",
        ":version",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# Compile-time attribute shims (EDC_PRINTF). Header-only and dependency-free on purpose:
# edc_error.h and edc_string_utils.h both use it, and neither should pull in edc_compat.h
# (which includes <windows.h>) merely to declare a function.
cc_library(
    name = "edc_attr",
    hdrs = ["edc_attr.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# <unistd.h> / <dirent.h> / <strings.h> for MSVC, on the include path for Windows builds
# only. Empty everywhere else, so Linux and macOS keep using the system headers and not one
# source file needs an #ifdef. `includes` is what puts win_compat/ on the compiler's search
# path; because it propagates, every target that depends on this one (or on something that
# does) can say `#include <unistd.h>` unchanged.
cc_library(
    name = "win_compat",
    srcs = select({
        # link()/symlink() need the Win32 API, which no shim header may include; they live in
        # this one .c file so <windows.h> stays out of the headers.
        "@platforms//os:windows": ["win_compat/win_compat.c"],
        "//conditions:default": [],
    }),
    hdrs = select({
        "@platforms//os:windows": [
            "win_compat/dirent.h",
            "win_compat/strings.h",
            "win_compat/unistd.h",
        ],
        "//conditions:default": [],
    }),
    includes = select({
        "@platforms//os:windows": ["win_compat"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

# EDC_LL_APPEND / EDC_LL_APPEND2: intrusive list append that takes the node type from the
# caller, so it needs neither __typeof nor decltype. Header-only, no dependencies.
cc_library(
    name = "edc_list",
    hdrs = ["edc_list.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# Header-only platform/GLib compatibility shim. Exposed on its own so very
# small binaries (e.g. //src/sandbox:sandbox_helper) can use it without pulling
# the whole //src/common:common library.
cc_library(
    name = "edc_compat",
    hdrs = ["edc_compat.h", "edc_utf8.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# Platform capabilities: the CHOICES (is there a kernel sandbox, which shell to spawn,
# how to start it) and the OPERATIONS whose implementation differs (mlock, signalling a
# process group). Its own target rather than part of //src/common:common, because
# edc_secret_lib needs it and common depends on edc_secret_lib -- putting it in common
# would close a cycle.
cc_library(
    name = "edc_platform",
    srcs = ["edc_platform.c"],
    hdrs = ["edc_platform.h"],
    includes = ["."],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_error",
    srcs = ["edc_error.c"],
    hdrs = ["edc_error.h"],
    includes = ["."],
    deps = [
        "//src/common:edc_attr",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# Path semantics (edc_path.h/.c) live in this target rather than their own: every one of
# these files is built on them, and edc_path.c needs nothing they do not already pull in.
cc_library(
    name = "edc_file_utils",
    srcs = [
        "edc_file_index.c",
        "edc_file_utils.c",
        "edc_path.c",
        "edc_path_guard.c",
    ],
    hdrs = [
        "edc_file_index.h",
        "edc_file_utils.h",
        "edc_path.h",
        "edc_path_guard.h",
    ],
    includes = ["."],
    deps = [
        "//src/common:edc_compat",
        "//src/common:edc_error",
        "//src/common:win_compat",
        "@libuv",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_string_utils",
    srcs = ["edc_string_utils.c"],
    hdrs = ["edc_string_utils.h"],
    includes = ["."],
    deps = [
        "//src/common:edc_attr",
        "//src/common:mbedtls_config",
        "//thirdparty/utf8proc",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "edc_datetime_utils",
    srcs = ["edc_datetime_utils.c"],
    hdrs = ["edc_datetime_utils.h"],
    includes = ["."],
    deps = [
        "//src/common:edc_compat",
        "@sds//:sds",
    ],
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
    includes = ["."],
    deps = [
        "//src/common:edc_error",
        "//src/common:edc_file_utils",
        "//src/common:edc_platform",
        "//src/common:mbedtls_config",
        "//src/common:win_compat",
        "@log.c//:log.c",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
)

# ── Bundled workspace ─────────────────────────────────────────────────────

# Generates a C file that carries the default workspace/ tree as a byte table,
# so the binary is self-contained and edc_init_env() does not need a
# `workspace/` directory on disk (see edc_bundled_workspace.h).
genrule(
    name = "workspace_embedded_c",
    srcs = ["//:workspace_files"],
    outs = ["workspace_embedded.c"],
    cmd = "$(execpath //tools:embed_workspace) --out $@ --prefix workspace --name edc_bundled_workspace $(SRCS)",
    cmd_bat = "$(execpath //tools:embed_workspace) --out $@ --prefix workspace --name edc_bundled_workspace $(SRCS)",
    tools = ["//tools:embed_workspace"],
    visibility = ["//visibility:public"],
)

# ── Combined core library (remaining utility modules) ─────────────────────

cc_library(
    name = "common",
    srcs = [
        ":workspace_embedded.c",
        "//:docs_embedded_c",
        "//:models_catalog_embedded_c",
        "edc_config.c",
        "edc_containers.c",
        "edc_crontab_jobs.c",
        "edc_endpoint_probe.c",
        "edc_https_client.c",
        "edc_init_env.c",
        "edc_log.c",
        "edc_payload.c",
        "edc_redact.c",
        "edc_semver.c",
        "edc_state_probe.c",
        "edc_update.c",
        "edc_upgrade.c",
    ],
    hdrs = [
        "edc_bundled_docs.h",
        "edc_bundled_files.h",
        "edc_bundled_workspace.h",
        "edc_bundled_models_catalog.h",
        "edc_compat.h",
        "edc_config.h",
        "edc_containers.h",
        "edc_https_client_internal.h",
        "edc_redact.h",
        "edc_semver.h",
        "edc_update.h",
        "edc_upgrade.h",
        "edc_crontab_jobs.h",
        "edc_endpoint_probe.h",
        "edc_https_client.h",
        "edc_init_env.h",
        "edc_log.h",
        "edc_msg_part.h",
        "edc_payload.h",
        "edc_state_probe.h",
        "edc_string.h",
    ],
    includes = ["."],
    deps = [
        "//src/common:edc_datetime_utils",
        "//src/common:edc_error",
        "//src/common:edc_file_utils",
        "//src/common:edc_list",
        "//src/common:edc_platform",
        "//src/common:win_compat",
        "//src/common:edc_secret_lib",
        "//src/common:edc_string_utils",
        "//src/common:mbedtls_config",
        "//thirdparty/utf8proc",
        "@cjson//:cjson",
        "@libuv",
        "@libyuarel//:libyuarel",
        "@llhttp//:llhttp",
        "@log.c//:log.c",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
        "@uthash//:uthash",
    ],
    # Debug builds keep trace/debug logging; normal builds clamp to INFO
    # (see edc_log_init). Same signal as the rest of the tree.
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
    linkstatic = True,
    visibility = ["//visibility:public"],
)
