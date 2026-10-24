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

def _get_sysroot_dirs(prefix, base_dir):
    target = prefix.rstrip("/").rsplit("/", 1)[-1].rstrip("-")
    return [
        base_dir + target + "/libc/usr/include/",
        base_dir + target + "/include/",
        base_dir + "include/",
    ]

def _generate_config_bzl(prefix, include_dirs, target_cpu, target_system_name, toolchain_identifier):
    tool_paths_entries = []
    for name in ["gcc", "g++", "ld", "ar", "cpp", "gcov", "nm", "objdump", "strip"]:
        tool_paths_entries.append(
            '        tool_path(name = "{}", path = prefix + "{}"),'.format(name, name),
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

    return """load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path", "action_config", "tool", "feature", "flag_set", "flag_group", "with_feature_set")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def _impl(ctx):
    prefix = "{prefix}"

    tool_paths = [
{tool_paths}
    ]

    action_configs = [
        action_config(
            action_name = "c++-compile",
            tools = [
                tool(path = prefix + "g++"),
            ],
        ),
        action_config(
            action_name = "c++-header-parsing",
            tools = [
                tool(path = prefix + "g++"),
            ],
        ),
        action_config(
            action_name = "c++-module-compile",
            tools = [
                tool(path = prefix + "g++"),
            ],
        ),
    ]

{all_compile_actions}
    features = [
{compilation_mode_features}
    ]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "{toolchain_identifier}",
        host_system_name = "local",
        target_system_name = "{target_system_name}",
        target_cpu = "{target_cpu}",
        target_libc = "unknown",
        compiler = "gcc",
        abi_version = "unknown",
        abi_libc_version = "unknown",
        tool_paths = tool_paths,
        action_configs = action_configs,
        features = features,
        cxx_builtin_include_directories = [
{include_dirs}
        ],
    )

aarch64_toolchain_config = rule(
    implementation = _impl,
    attrs = {{}},
)
""".format(prefix = prefix, tool_paths = tool_paths_str, include_dirs = include_dirs_str, target_cpu = target_cpu, target_system_name = target_system_name, toolchain_identifier = toolchain_identifier, all_compile_actions = COMPILATION_MODE_ACTIONS, compilation_mode_features = COMPILATION_MODE_FEATURES + COVERAGE_FEATURE)

def _aarch64_toolchain_repo_impl(rctx):
    prefix = rctx.os.environ.get("CROSS_COMPILE", "")
    cross = bool(prefix)

    if not prefix:
        gcc_bin = rctx.which("gcc")
        if gcc_bin:
            prefix = str(gcc_bin)[:str(gcc_bin).rfind("/")] + "/"

    null_device = "NUL" if rctx.os.name.startswith("windows") else "/dev/null"
    result = rctx.execute(
        [prefix + "gcc", "-E", "-v", "-x", "c", null_device],
        environment = {"LC_ALL": "C"},
    )
    if result.return_code != 0:
        fail("Failed to query compiler: " + result.stderr)

    include_dirs = _parse_builtin_includes(result.stderr)

    target_cpu = "aarch64"
    target_system_name = "aarch64-none-linux-gnu"
    toolchain_identifier = "aarch64-none-linux-gnu"

    if cross:
        base_dir = prefix[:prefix.rfind("/bin/")] + "/"
        include_dirs.extend(_get_sysroot_dirs(prefix, base_dir))

    rctx.file("aarch64_compiler_config.bzl", content = _generate_config_bzl(prefix, include_dirs, target_cpu, target_system_name, toolchain_identifier))

    target_compat = '''
        target_compatible_with = [
            "@platforms//os:linux",
            "@platforms//cpu:aarch64",
        ],'''

    rctx.file("BUILD", content = """load(":aarch64_compiler_config.bzl", "aarch64_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

aarch64_toolchain_config(name = "aarch64_config")

filegroup(name = "empty")

cc_toolchain(
    name = "aarch64_cc_toolchain",
    toolchain_config = ":aarch64_config",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
)

toolchain(
    name = "aarch64_linux_toolchain",
    exec_compatible_with = [
    ],{target_compat}
    toolchain = ":aarch64_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(target_compat = target_compat))

aarch64_toolchain_repo = repository_rule(
    implementation = _aarch64_toolchain_repo_impl,
    environ = ["CROSS_COMPILE"],
)
