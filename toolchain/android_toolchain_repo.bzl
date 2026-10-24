load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load(
    ":compilation_mode_features.bzl",
    "COMPILATION_MODE_ACTIONS",
    "COMPILATION_MODE_FEATURES",
    "COVERAGE_FEATURE",
)

def _normalize_path(path):
    parts = path.split("/")
    result = []
    for p in parts:
        if p == ".." and result:
            result.pop()
        elif p and p != ".":
            result.append(p)
    if not result:
        return "/"
    return "/" + "/".join(result)

def _parse_builtin_includes(stderr):
    dirs = []
    in_list = False
    for line in stderr.split("\n"):
        stripped = line.strip()
        if stripped == "#include <...> search starts here:":
            in_list = True
            continue
        if stripped == "End of search list.":
            break
        if in_list:
            normalized = _normalize_path(stripped)
            if stripped not in dirs:
                dirs.append(stripped)
            if normalized not in dirs:
                dirs.append(normalized)
    return dirs

def _generate_android_config_bzl(tool_paths, include_dirs, arch_config):
    tool_paths_entries = []
    for name in ["gcc", "g++", "ld", "ar", "cpp", "gcov", "nm", "objdump", "strip"]:
        path = tool_paths.get(name, "")
        tool_paths_entries.append(
            '        tool_path(name = "{}", path = "{}"),'.format(name, path),
        )
    tool_paths_str = "\n".join(tool_paths_entries)

    seen = {}
    unique_dirs = []
    for d in include_dirs:
        if d not in seen:
            seen[d] = True
            unique_dirs.append(d)

    include_entries = []
    for d in unique_dirs:
        include_entries.append('            "{}",'.format(d))
    include_dirs_str = "\n".join(include_entries)

    # Always include the compilation-mode feature set (see
    # :compilation_mode_features.bzl) so --compilation_mode actually maps to
    # -O flags; arch_config may add its own features on top.
    features_str = COMPILATION_MODE_FEATURES + COVERAGE_FEATURE
    if arch_config.get("features"):
        features_str = COMPILATION_MODE_FEATURES + COVERAGE_FEATURE + arch_config["features"]
    cc_toolchain_config_imports = 'load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path", "action_config", "tool", "feature", "flag_set", "flag_group", "with_feature_set")'

    return """{cc_toolchain_config_imports}
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def _impl(ctx):
    tool_paths = [
{tool_paths}
    ]

    action_configs = [
        action_config(
            action_name = "c++-compile",
            tools = [
                tool(path = "g++"),
            ],
        ),
        action_config(
            action_name = "c++-header-parsing",
            tools = [
                tool(path = "g++"),
            ],
        ),
        action_config(
            action_name = "c++-module-compile",
            tools = [
                tool(path = "g++"),
            ],
        ),
    ]

{all_compile_actions}
    features = [
{features}
    ]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "{toolchain_identifier}",
        host_system_name = "local",
        target_system_name = "{target_system_name}",
        target_cpu = "{target_cpu}",
        target_libc = "bionic",
        compiler = "clang",
        abi_version = "{abi_version}",
        abi_libc_version = "unknown",
        tool_paths = tool_paths,
        action_configs = action_configs,
        features = features,
        cxx_builtin_include_directories = [
{include_dirs}
        ],
    )

android_toolchain_config = rule(
    implementation = _impl,
    attrs = {{}},
)
""".format(
        cc_toolchain_config_imports = cc_toolchain_config_imports,
        tool_paths = tool_paths_str,
        include_dirs = include_dirs_str,
        features = features_str,
        all_compile_actions = COMPILATION_MODE_ACTIONS,
        toolchain_identifier = arch_config["toolchain_identifier"],
        target_system_name = arch_config["target_system_name"],
        target_cpu = arch_config["target_cpu"],
        abi_version = arch_config["abi_version"],
    )

def _get_ndk_host_tag(rctx):
    os_name = rctx.os.name
    if "mac" in os_name:
        return "darwin-x86_64"
    arch = rctx.os.arch
    if arch == "aarch64":
        return "linux-aarch64"
    return "linux-x86_64"

def _android_toolchain_repo_impl(rctx, arch_config):
    ndk = _find_ndk(rctx)
    if not ndk:
        _create_dummy_repo(rctx, arch_config)
        return

    api_level = rctx.os.environ.get("ANDROID_API_LEVEL", "21")
    host_tag = _get_ndk_host_tag(rctx)
    bin_dir = ndk.rstrip("/") + "/toolchains/llvm/prebuilt/" + host_tag + "/bin/"

    target_triple = arch_config["target_triple"]
    clang_bin = bin_dir + target_triple + api_level + "-clang"

    null_device = "NUL" if rctx.os.name.startswith("windows") else "/dev/null"
    result = rctx.execute(
        [clang_bin, "-E", "-v", "-x", "c", null_device],
        environment = {"LC_ALL": "C"},
    )
    if result.return_code != 0:
        _create_dummy_repo(rctx, arch_config)
        return

    include_dirs = _parse_builtin_includes(result.stderr)

    tool_paths = {
        "gcc": clang_bin,
        "g++": clang_bin,
        "ld": bin_dir + "ld.lld",
        "ar": bin_dir + "llvm-ar",
        "cpp": clang_bin,
        "gcov": bin_dir + "llvm-cov",
        "nm": bin_dir + "llvm-nm",
        "objdump": bin_dir + "llvm-objdump",
        "strip": bin_dir + "llvm-strip",
    }

    rctx.file(
        "android_compiler_config.bzl",
        content = _generate_android_config_bzl(tool_paths, include_dirs, arch_config),
    )

    target_compat = '''
        target_compatible_with = [
            "@platforms//os:android",
            "@platforms//cpu:{cpu}",
        ],'''.format(cpu = arch_config["platform_cpu"])

    rctx.file("BUILD", content = """load(":android_compiler_config.bzl", "android_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

android_toolchain_config(name = "android_config")

filegroup(name = "empty")

cc_toolchain(
    name = "android_cc_toolchain",
    toolchain_config = ":android_config",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
)

toolchain(
    name = "{toolchain_name}",
    exec_compatible_with = [
    ],{target_compat}
    toolchain = ":android_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(
        toolchain_name = arch_config["toolchain_name"],
        target_compat = target_compat,
    ))

def _find_ndk(rctx):
    ndk = rctx.os.environ.get("ANDROID_NDK_HOME", "")
    if ndk:
        return ndk
    sdk_root = rctx.os.environ.get("ANDROID_SDK_ROOT", "")
    if sdk_root:
        return sdk_root.rstrip("/") + "/ndk-bundle"
    android_home = rctx.os.environ.get("ANDROID_HOME", "")
    if android_home:
        return android_home.rstrip("/") + "/ndk-bundle"
    return None

def _create_dummy_repo(rctx, arch_config):
    rctx.file("android_compiler_config.bzl", content = """
def _impl(ctx):
    fail("Android NDK not found - set ANDROID_NDK_HOME environment variable")
android_toolchain_config = rule(implementation = _impl, attrs = {})
""")
    target_compat = '''
        target_compatible_with = [
            "@platforms//os:android",
            "@platforms//cpu:{cpu}",
        ],'''.format(cpu = arch_config["platform_cpu"])
    rctx.file("BUILD", content = """load(":android_compiler_config.bzl", "android_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

android_toolchain_config(name = "android_config")

filegroup(name = "empty")

cc_toolchain(
    name = "android_cc_toolchain",
    toolchain_config = ":android_config",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
)

toolchain(
    name = "{toolchain_name}",
    exec_compatible_with = [
    ],{target_compat}
    toolchain = ":android_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(
        toolchain_name = arch_config["toolchain_name"],
        target_compat = target_compat,
    ))

# --- AArch64 Android ---

_AARCH64_ANDROID_CONFIG = {
    "toolchain_name": "aarch64_android_toolchain",
    "toolchain_identifier": "aarch64-linux-android",
    "target_system_name": "aarch64-linux-android",
    "target_cpu": "aarch64",
    "platform_cpu": "aarch64",
    "abi_version": "21",
    "target_triple": "aarch64-linux-android",
}

def _aarch64_android_toolchain_repo_impl(rctx):
    _android_toolchain_repo_impl(rctx, _AARCH64_ANDROID_CONFIG)

aarch64_android_toolchain_repo = repository_rule(
    implementation = _aarch64_android_toolchain_repo_impl,
    environ = ["ANDROID_NDK_HOME", "ANDROID_SDK_ROOT", "ANDROID_HOME", "ANDROID_API_LEVEL"],
)

# --- ARMv7 Android ---

_ARM_ANDROID_FEATURES = """        feature(
            name = "armv7_arch_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = [
                        "c-compile",
                        "c++-compile",
                        "assemble",
                        "preprocess-assemble",
                    ],
                    flag_groups = [
                        flag_group(
                            flags = ["-march=armv7-a"],
                        ),
                    ],
                ),
            ],
        ),"""

_ARM_ANDROID_CONFIG = {
    "toolchain_name": "arm_android_toolchain",
    "toolchain_identifier": "armv7a-linux-androideabi",
    "target_system_name": "armv7a-linux-androideabi",
    "target_cpu": "arm",
    "platform_cpu": "armv7",
    "abi_version": "21",
    "target_triple": "armv7a-linux-androideabi",
    "features": _ARM_ANDROID_FEATURES,
}

def _arm_android_toolchain_repo_impl(rctx):
    _android_toolchain_repo_impl(rctx, _ARM_ANDROID_CONFIG)

arm_android_toolchain_repo = repository_rule(
    implementation = _arm_android_toolchain_repo_impl,
    environ = ["ANDROID_NDK_HOME", "ANDROID_SDK_ROOT", "ANDROID_HOME", "ANDROID_API_LEVEL"],
)
