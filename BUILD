load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")

config_setting(
    name = "debug",
    values = {"compilation_mode": "dbg"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "macos",
    constraint_values = [
        "@platforms//os:macos"
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "macos_debug",
    constraint_values = [
        "@platforms//os:macos"
    ],
    values = {"compilation_mode": "dbg"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "linux_debug",
    constraint_values = [
        "@platforms//os:linux"
    ],
    values = {"compilation_mode": "dbg"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "linux",
    constraint_values = [
        "@platforms//os:linux"
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "windows",
    constraint_values = [
        "@platforms//os:windows"
    ],
    visibility = ["//visibility:public"],
)

platform(
    name = "arm_linux",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:armv7",
    ],
)

platform(
    name = "aarch64_linux",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:aarch64",
    ],
)

platform(
    name = "aarch64_android",
    constraint_values = [
        "@platforms//os:android",
        "@platforms//cpu:aarch64",
    ],
)

platform(
    name = "arm_android",
    constraint_values = [
        "@platforms//os:android",
        "@platforms//cpu:armv7",
    ],
)

platform(
    name = "aarch64_macos",
    constraint_values = [
        "@platforms//os:macos",
        "@platforms//cpu:aarch64",
    ],
)

platform(
    name = "amd64_linux",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

platform(
    name = "amd64_windows",
    constraint_values = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

config_setting(
    name = "is_arm_linux",
    constraint_values = [],
    values = {"platforms": "//:arm_linux"}, # 直接匹配命令行参数
)

config_setting(
    name = "is_aarch64_linux",
    constraint_values = [],
    values = {"platforms": "//:aarch64_linux"},
)

config_setting(
    name = "is_amd64_linux",
    constraint_values = [],
    values = {"platforms": "//:amd64_linux"},
)

config_setting(
    name = "is_aarch64_macos",
    constraint_values = [],
    values = {"platforms": "//:aarch64_macos"},
)

config_setting(
    name = "is_aarch64_android",
    constraint_values = [],
    values = {"platforms": "//:aarch64_android"},
)

config_setting(
    name = "is_arm_android",
    constraint_values = [],
    values = {"platforms": "//:arm_android"},
)

config_setting(
    name = "android",
    constraint_values = [
        "@platforms//os:android",
    ],
    visibility = ["//visibility:public"],
)

FILE_NAME = select({
        ":is_arm_linux": "edgeclaw_arm_linux",
        ":is_aarch64_linux": "edgeclaw_aarch64_linux",
        ":is_amd64_linux": "edgeclaw_amd64_linux",
        ":is_aarch64_macos": "edgeclaw_aarch64_macos",
        "//conditions:default": "edgeclaw_unkown",
})

DAGENT_FILE_NAME = select({
        ":is_arm_linux": "device_agent_arm_linux",
        ":is_aarch64_linux": "device_agent_aarch64_linux",
        ":is_amd64_linux": "device_agent_amd64_linux",
        ":is_aarch64_macos": "device_agent_aarch64_macos",
        ":is_aarch64_android": "device_agent_aarch64_android",
        ":is_arm_android": "device_agent_arm_android",
        "//conditions:default": "device_agent_unkown",
})

pkg_files(
    name = "edgeclaw_binary",
    srcs = ["//src:edgeclaw"],
    attributes = pkg_attributes(mode = "0755"), 
)


WORKSPACE_FILES = glob(["workspace/**"])

pkg_files(
    name = "edgeclaw_workspace",
    srcs = WORKSPACE_FILES,
    attributes = pkg_attributes(mode = "0644"),
    strip_prefix = "workspace",
    prefix = "workspace",
)

pkg_tar(
    name = "edgeclaw",
    srcs = [
        ":edgeclaw_binary",
        ":edgeclaw_workspace",
    ],
    extension = "tar.gz",
    package_dir = FILE_NAME,
    package_file_name = FILE_NAME + ".tar.gz",
)

pkg_files(
    name = "device_agent_binary",
    srcs = ["//src:device_agent"],
    attributes = pkg_attributes(mode = "0755"),
)

pkg_tar(
    name = "device_agent",
    srcs = [
        ":device_agent_binary",
    ],
    extension = "tar.gz",
    package_dir = DAGENT_FILE_NAME,
    package_file_name = DAGENT_FILE_NAME + ".tar.gz",
)
