load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

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

    return """load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path", "feature", "flag_set", "flag_group", "action_config", "tool")
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

    features = [
        feature(
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
        ),
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

arm_toolchain_config = rule(
    implementation = _impl,
    attrs = {{}},
)
""".format(prefix = prefix, tool_paths = tool_paths_str, include_dirs = include_dirs_str, target_cpu = target_cpu, target_system_name = target_system_name, toolchain_identifier = toolchain_identifier)

def _arm_toolchain_repo_impl(rctx):
    prefix = rctx.os.environ.get("CROSS_COMPILE", "")
    cross = bool(prefix)

    if not prefix:
        gcc_bin = rctx.which("gcc")
        if gcc_bin:
            prefix = str(gcc_bin)[:str(gcc_bin).rfind("/")] + "/"

    null_device = "NUL" if rctx.os.name.startswith("windows") else "/dev/null"
    compiler_args = [prefix + "gcc", "-E", "-v", "-x", "c", null_device]
    result = rctx.execute(compiler_args, environment = {"LC_ALL": "C"})
    if result.return_code != 0:
        fail("Failed to query compiler: " + result.stderr)

    include_dirs = _parse_builtin_includes(result.stderr)

    target_cpu = "arm"
    target_system_name = "arm-none-linux-gnueabihf"
    toolchain_identifier = "arm-none-linux-gnueabihf"

    if cross:
        base_dir = prefix[:prefix.rfind("/bin/")] + "/"
        include_dirs.extend(_get_sysroot_dirs(prefix, base_dir))

    rctx.file("arm_compiler_config.bzl", content = _generate_config_bzl(prefix, include_dirs, target_cpu, target_system_name, toolchain_identifier))

    target_compat = '''
        target_compatible_with = [
            "@platforms//os:linux",
            "@platforms//cpu:armv7",
        ],'''

    rctx.file("BUILD", content = """load(":arm_compiler_config.bzl", "arm_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

arm_toolchain_config(name = "arm_config")

filegroup(name = "empty")

cc_toolchain(
    name = "arm_cc_toolchain",
    toolchain_config = ":arm_config",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
)

toolchain(
    name = "arm_linux_toolchain",
    exec_compatible_with = [
        "@platforms//os:linux",
    ],{target_compat}
    toolchain = ":arm_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(target_compat = target_compat))

arm_toolchain_repo = repository_rule(
    implementation = _arm_toolchain_repo_impl,
    environ = ["CROSS_COMPILE"],
)
