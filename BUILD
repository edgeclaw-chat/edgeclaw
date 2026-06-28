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

FILE_NAME = select({
        ":is_arm_linux": "edgeclaw_arm_linux",
        ":is_aarch64_linux": "edgeclaw_aarch64_linux",
        ":is_amd64_linux": "edgeclaw_amd64_linux",
        ":is_aarch64_macos": "edgeclaw_aarch64_macos",
        "//conditions:default": "edgeclaw_unkown",
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
