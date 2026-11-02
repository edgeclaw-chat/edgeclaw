load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load(
    ":compilation_mode_features.bzl",
    "COMPILATION_MODE_ACTIONS",
    "COMPILATION_MODE_FEATURES",
    "COVERAGE_FEATURE",
)

def _normalize_path(path):
    parts = path.replace("\\", "/").split("/")
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

def _find_tool(rctx, bin_dir, tool_prefix, name):
    full_name = tool_prefix + name
    found = rctx.which(full_name)
    if found:
        return str(found)
    exe_path = bin_dir + full_name + ".exe"
    if rctx.path(exe_path).exists:
        return exe_path
    return None

def _generate_config_bzl(bin_dir, tool_prefix, include_dirs, target_cpu, target_system_name, toolchain_identifier):
    tool_names = ["gcc", "g++", "ld", "ar", "cpp", "gcov", "nm", "objdump", "strip"]

    tool_paths_entries = []
    for name in tool_names:
        tool_paths_entries.append(
            '        tool_path(name = "{}", path = "{}"),'.format(name, bin_dir + tool_prefix + name),
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

    cxx_tool_path = bin_dir + tool_prefix + "g++"

    return """load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path", "action_config", "tool", "feature", "flag_set", "flag_group", "with_feature_set", "artifact_name_pattern")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def _impl(ctx):
    tool_paths = [
{tool_paths}
    ]

    action_configs = [
        action_config(
            action_name = "c++-compile",
            tools = [
                tool(path = "{cxx_tool_path}"),
            ],
        ),
        action_config(
            action_name = "c++-header-parsing",
            tools = [
                tool(path = "{cxx_tool_path}"),
            ],
        ),
        action_config(
            action_name = "c++-module-compile",
            tools = [
                tool(path = "{cxx_tool_path}"),
            ],
        ),
    ]

{all_compile_actions}
    artifact_name_patterns = [
        artifact_name_pattern(
            category_name = "executable",
            prefix = "",
            extension = ".exe",
        ),
    ]

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
        compiler = "mingw",
        abi_version = "unknown",
        abi_libc_version = "unknown",
        tool_paths = tool_paths,
        action_configs = action_configs,
        artifact_name_patterns = artifact_name_patterns,
        features = features,
        cxx_builtin_include_directories = [
{include_dirs}
        ],
    )

mingw_toolchain_config = rule(
    implementation = _impl,
    attrs = {{}},
)
""".format(tool_paths = tool_paths_str, include_dirs = include_dirs_str, target_cpu = target_cpu, target_system_name = target_system_name, toolchain_identifier = toolchain_identifier, all_compile_actions = COMPILATION_MODE_ACTIONS, compilation_mode_features = COMPILATION_MODE_FEATURES + COVERAGE_FEATURE, cxx_tool_path = cxx_tool_path)

def _generate_stub(rctx):
    rctx.file("mingw_compiler_config.bzl", content = """load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
def _impl(ctx):
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "stub",
        host_system_name = "local",
        target_system_name = "unknown",
        target_cpu = "unknown",
        target_libc = "unknown",
        compiler = "unknown",
    )
mingw_toolchain_config = rule(implementation = _impl, attrs = {})
""")
    rctx.file("BUILD", content = """load(":mingw_compiler_config.bzl", "mingw_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

mingw_toolchain_config(name = "mingw_config")
filegroup(name = "empty")

cc_toolchain(
    name = "mingw_cc_toolchain",
    toolchain_config = ":mingw_config",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
)

toolchain(
    name = "mingw_windows_toolchain",
    exec_compatible_with = [
        "@platforms//os:windows",
    ],
    target_compatible_with = [
        "@platforms//os:none",
    ],
    toolchain = ":mingw_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""")

def _find_mingw_gcc(rctx):
    env_prefix = rctx.os.environ.get("MINGW_PREFIX", "")
    if env_prefix:
        env_prefix = env_prefix.replace("\\", "/")
        if not env_prefix.endswith("/"):
            env_prefix += "/"
        if env_prefix.endswith("-"):
            return (env_prefix, "")
        gcc_exe = env_prefix + "gcc.exe"
        if rctx.path(gcc_exe).exists:
            return (env_prefix, "")
        gcc_exe2 = env_prefix + "x86_64-w64-mingw32-gcc.exe"
        if rctx.path(gcc_exe2).exists:
            return (env_prefix, "x86_64-w64-mingw32-")
        return ("", "")

    gcc_bin = rctx.which("gcc")
    if gcc_bin:
        gcc_str = str(gcc_bin).replace("\\", "/")
        idx = gcc_str.rfind("/")
        return (gcc_str[:idx + 1], "")

    for tool_prefix in ["x86_64-w64-mingw32-", "i686-w64-mingw32-"]:
        gcc = rctx.which(tool_prefix + "gcc")
        if gcc:
            gcc_str = str(gcc).replace("\\", "/")
            idx = gcc_str.rfind("/")
            return (gcc_str[:idx + 1], tool_prefix)

    for root in ["C:/", "D:/", "E:/"]:
        for msys_dir in ["msys64", "msys2"]:
            for arch_dir in ["mingw64", "mingw32"]:
                bin_dir = root + msys_dir + "/" + arch_dir + "/bin/"
                gcc_path = bin_dir + "gcc.exe"
                if rctx.path(gcc_path).exists:
                    return (bin_dir, "")

    return ("", "")

def _mingw_toolchain_repo_impl(rctx):
    if not rctx.os.name.startswith("windows"):
        _generate_stub(rctx)
        return

    bin_dir, tool_prefix = _find_mingw_gcc(rctx)

    if not bin_dir:
        print("WARNING: MinGW-w64 GCC not found. Searched PATH and common MSYS2/MinGW-w64 installation directories (C:/msys64/mingw64, etc.). Install MSYS2 (https://www.msys2.org/) or add MinGW gcc to PATH. Or set MINGW_PREFIX to the bin directory containing gcc.")  # buildifier: disable=print
        _generate_stub(rctx)
        return

    gcc_name = tool_prefix + "gcc"
    null_device = "NUL"
    result = rctx.execute(
        [bin_dir + gcc_name, "-E", "-v", "-x", "c", null_device],
        environment = {"LC_ALL": "C"},
    )
    if result.return_code != 0:
        _generate_stub(rctx)
        return

    include_dirs = _parse_builtin_includes(result.stderr)

    target_cpu = "x86_64"
    target_system_name = "x86_64-w64-mingw32"
    toolchain_identifier = "x86_64-w64-mingw32"

    rctx.file("mingw_compiler_config.bzl", content = _generate_config_bzl(bin_dir, tool_prefix, include_dirs, target_cpu, target_system_name, toolchain_identifier))

    rctx.file("BUILD", content = """load(":mingw_compiler_config.bzl", "mingw_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

mingw_toolchain_config(name = "mingw_config")

filegroup(name = "empty")

cc_toolchain(
    name = "mingw_cc_toolchain",
    toolchain_config = ":mingw_config",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
)

toolchain(
    name = "mingw_windows_toolchain",
    exec_compatible_with = [
        "@platforms//os:windows",
    ],
    target_compatible_with = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
    toolchain = ":mingw_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""")

mingw_toolchain_repo = repository_rule(
    implementation = _mingw_toolchain_repo_impl,
    environ = ["MINGW_PREFIX", "PATH"],
)
