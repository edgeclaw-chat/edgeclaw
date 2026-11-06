"""A musl-libc cc_toolchain, used only to produce the fully static release binaries.

Why this exists separately from aarch64_toolchain_repo.bzl / arm_toolchain_repo.bzl:
the development loop builds against the host glibc and stays that way. musl is an
OPT-IN, selected by `--config=musl_<arch>` (see .bazelrc), which sets both
`--platforms=//:<arch>_linux_musl` and `--extra_toolchains=@musl_toolchain_<arch>//...`.
Nothing here is registered through `register_toolchains()`, so a machine without a
musl toolchain keeps building exactly as before.

Three rules govern this file, each learned the hard way:

1. A missing toolchain must NOT fail at repository time. This repo rule runs on
   every `bazel build`, including plain glibc ones, so a `fail()` here would take
   down the whole development loop on a machine that only wants to build natively.
   When nothing is found we emit a placeholder toolchain whose "compiler" prints
   an explanation and exits 1 -- the failure surfaces only if you actually ask for
   musl.

2. "The binary exists" is not the same as "the binary runs here". musl.cc publishes
   host-specific packages, and /opt often holds both the x86-host and the native
   build of the same version. Executing `gcc -dumpmachine` is the only check that
   separates them; a file-existence check happily accepts an i386 ELF on aarch64.

3. A binary toolchain can be incomplete. /opt/aarch64-linux-musl-native is a native
   aarch64 gcc that ships binutils under UNPREFIXED names only (`ar`, `ld`, ...) and
   has no `aarch64-linux-musl-ar`, which Bazel's tool_path layout requires. That
   shows up much later as `execvp(...-ar): No such file or directory` in the middle
   of a build, so every tool is probed up front instead.
"""

load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load(
    ":compilation_mode_features.bzl",
    "COMPILATION_MODE_ACTIONS",
    "COMPILATION_MODE_FEATURES",
    "COVERAGE_FEATURE",
)

# Per-arch facts, in one place. `cpu` is what Bazel calls the CPU in tool paths and
# target_cpu; `cpu_constraint` is the platform constraint it must agree with (note
# that Bazel's `arm` is `armv7` as a constraint).
_MUSL_ARCHES = {
    "aarch64": {
        "triple": "aarch64-linux-musl",
        # musl-cross (github.com/musl-cross/musl-cross) spells its triples the GNU way and
        # ships them as GitHub release assets -- which matters, because musl.cc is not
        # routable from a GitHub runner. Same compiler and same layout
        # (<triple>/bin/<triple>-gcc), only the name differs, so both spellings are searched.
        # `triple` itself stays canonical: it is what target_system_name reports and what
        # MUSL_CROSS_COMPILE_AARCH64 is documented against.
        "alt_triples": ["aarch64-unknown-linux-musl"],
        "cpu": "aarch64",
        "cpu_constraint": "@platforms//cpu:aarch64",
    },
    "arm": {
        # musleabi (soft-float), matching what the glibc release actually links: the matrix
        # uses CROSS_COMPILE=/usr/bin/arm-linux-gnueabi-, which is the soft-float EABI
        # toolchain (verified: a binary from it carries EF_ARM_ABI_FLOAT_SOFT, e_flags
        # 0x5000200). The `arm-none-linux-gnueabihf` string in arm_toolchain_repo.bzl is
        # only a toolchain_identifier label -- it does not describe the compiler, and
        # reading it as if it did is what had this pointing at musleabihf.
        #
        # Using musleabihf here would ship two arm packages for different ABIs, where only
        # one can run on a given device.
        "triple": "arm-linux-musleabi",
        "alt_triples": ["arm-unknown-linux-musleabi"],
        "cpu": "arm",
        "cpu_constraint": "@platforms//cpu:armv7",
    },
    "amd64": {
        "triple": "x86_64-linux-musl",
        "alt_triples": ["x86_64-unknown-linux-musl"],
        "cpu": "x86_64",
        "cpu_constraint": "@platforms//cpu:x86_64",
    },
}

# tool_path() entries the config emits, and therefore the tools that must exist.
_REQUIRED_TOOLS = ["gcc", "g++", "ld", "ar", "cpp", "gcov", "nm", "objdump", "strip"]

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
    """Pull `#include <...>` search roots out of a `gcc -E -v` dump."""
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
    for name in _REQUIRED_TOOLS:
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
        target_libc = "musl",
        compiler = "gcc",
        abi_version = "musl",
        abi_libc_version = "musl",
        tool_paths = tool_paths,
        action_configs = action_configs,
        features = features,
        cxx_builtin_include_directories = [
{include_dirs}
        ],
    )

musl_toolchain_config = rule(
    implementation = _impl,
    attrs = {{}},
)
""".format(
        prefix = prefix,
        tool_paths = tool_paths_str,
        include_dirs = include_dirs_str,
        target_cpu = target_cpu,
        target_system_name = target_system_name,
        toolchain_identifier = toolchain_identifier,
        all_compile_actions = COMPILATION_MODE_ACTIONS,
        compilation_mode_features = COMPILATION_MODE_FEATURES + COVERAGE_FEATURE,
    )

def _probe(rctx, prefix):
    """Return why `prefix` is unusable, or None if it is a working musl toolchain.

    Checks the tools exist AND that this host can execute them; see the header.
    """
    for name in _REQUIRED_TOOLS:
        if not rctx.path(prefix + name).exists:
            return "missing " + prefix + name

    cc = prefix + "gcc"
    dump = rctx.execute([cc, "-dumpmachine"], environment = {"LC_ALL": "C"})
    if dump.return_code != 0:
        return "cannot execute {} (wrong host arch?): {}".format(
            cc,
            (dump.stderr or dump.stdout).strip(),
        )
    if "musl" not in dump.stdout:
        return "{} targets {}, not musl".format(cc, dump.stdout.strip())
    return None

def _candidate_prefixes(rctx, arch, meta):
    """Prefixes to try, best first. `prefix` is everything up to and including the
    trailing dash, i.e. "gcc" is appended to get the compiler."""
    env_name = "MUSL_CROSS_COMPILE_" + arch.upper()

    # An explicit override is taken as the answer, not as a hint: if it is unusable
    # we say why rather than silently building with a different toolchain than the
    # one asked for. Falling back here would make a mis-set variable look like it
    # worked, and the resulting binary would not be the one the user configured.
    explicit = rctx.os.environ.get(env_name, "")
    if explicit:
        return env_name, [explicit]

    candidates = []

    # Canonical spelling first, then the GNU-style alternates a different distribution
    # may use. Searching both means "unpack the tarball under /opt" keeps working
    # whichever upstream the tarball came from.
    for triple in [meta["triple"]] + meta.get("alt_triples", []):
        for dir in [
            # musl-cross-make installed under /opt: <prefix>/bin/<triple>-gcc.
            "/opt/{}/bin/".format(triple),
            # musl-cross-make's "native" variant (same layout, separate tree).
            "/opt/{}-native/bin/".format(triple),
            # The musl.cc tarball: <triple>-cross/bin/.
            "/opt/{}-cross/bin/".format(triple),
            "/opt/{}-cross/{}/bin/".format(triple, triple),
        ]:
            candidates.append(dir + triple + "-")

        # Anything already on PATH (the usual case for a distro musl package).
        found = rctx.which(triple + "-gcc")
        if found:
            candidates.append(str(found)[:str(found).rfind("/")] + "/" + triple + "-")

    return env_name, candidates

def _write_found(rctx, arch, meta, prefix, include_dirs):
    target_cpu = meta["cpu"]
    triple = meta["triple"]
    target_system_name = triple
    toolchain_identifier = triple

    rctx.file(
        "musl_compiler_config.bzl",
        content = _generate_config_bzl(prefix, include_dirs, target_cpu, target_system_name, toolchain_identifier),
    )

    config_name = "{}_musl_config".format(arch)
    cc_toolchain_name = "{}_musl_cc_toolchain".format(arch)
    toolchain_name = "{}_linux_musl_toolchain".format(arch)

    rctx.file("BUILD", content = """load(":musl_compiler_config.bzl", "musl_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

musl_toolchain_config(name = "{config_name}")

filegroup(name = "empty")

cc_toolchain(
    name = "{cc_toolchain_name}",
    toolchain_config = ":{config_name}",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
)

# `libc:musl` is what keeps this toolchain from ever being selected for a normal
# glibc build: only //:<arch>_linux_musl carries that constraint value.
toolchain(
    name = "{toolchain_name}",
    exec_compatible_with = [],
    target_compatible_with = [
        "@platforms//os:linux",
        "{cpu_constraint}",
        "@edgeclaw//:libc_musl",
    ],
    toolchain = ":{cc_toolchain_name}",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(
        config_name = config_name,
        cc_toolchain_name = cc_toolchain_name,
        toolchain_name = toolchain_name,
        cpu_constraint = meta["cpu_constraint"],
    ))

def _write_missing(rctx, arch, env_name, reasons):
    """Emit a toolchain that fails loudly, but only when musl is actually requested."""
    explanation = """#!/bin/sh
cat >&2 <<'EOF'
===============================================================================
No usable musl toolchain was found for {arch}.

Tried, and why each was rejected:
{reasons}

A musl toolchain must have all of {tools}
and it must be built for THIS host architecture (an x86-host package cannot run
on aarch64 -- `gcc -dumpmachine` is what rejects it).

Point Bazel at yours with:
    export {env_name}=/path/to/bin/<triple>-
(or unpack a musl-cross tarball under /opt and it is found automatically: the
searched layouts are /opt/<triple>/bin/ and /opt/<triple>-cross/bin/, and both
the `<arch>-linux-musl` and `<arch>-unknown-linux-musl` spellings are tried)

Then rebuild with --config=musl_{arch}.
This does not affect a normal build: without that flag nothing here is used.
===============================================================================
EOF
exit 1
""".format(
        arch = arch,
        env_name = env_name,
        tools = ", ".join(_REQUIRED_TOOLS),
        reasons = "\n".join(["  - " + r for r in reasons] if reasons else ["  - nothing on PATH, /opt or " + env_name]),
    )

    rctx.file("musl_missing.sh", content = explanation, executable = True)

    config_name = "{}_musl_config".format(arch)
    cc_toolchain_name = "{}_musl_cc_toolchain".format(arch)
    toolchain_name = "{}_linux_musl_toolchain".format(arch)

    # Every tool path is the same script: the build stops at the first compile with
    # the explanation above, instead of failing here and breaking everyone's build.
    tool_lines = "\n".join([
        '        tool_path(name = "{}", path = "musl_missing.sh"),'.format(name)
        for name in _REQUIRED_TOOLS
    ])

    rctx.file("musl_compiler_config.bzl", content = """load("@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

def _impl(ctx):
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "{toolchain_identifier}",
        host_system_name = "local",
        target_system_name = "{triple}",
        target_cpu = "{cpu}",
        target_libc = "musl",
        compiler = "gcc",
        abi_version = "musl",
        abi_libc_version = "musl",
        tool_paths = [
{tool_lines}
        ],
    )

musl_toolchain_config = rule(
    implementation = _impl,
    attrs = {{}},
)
""".format(
        toolchain_identifier = "{}_musl_MISSING".format(arch),
        triple = _MUSL_ARCHES[arch]["triple"],
        cpu = _MUSL_ARCHES[arch]["cpu"],
        tool_lines = tool_lines,
    ))

    rctx.file("BUILD", content = """load(":musl_compiler_config.bzl", "musl_toolchain_config")
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

package(default_visibility = ["//visibility:public"])

musl_toolchain_config(name = "{config_name}")

# Unlike the real toolchains (which point at absolute host paths and so need no
# files), the placeholder script lives inside this repo and must therefore be
# declared as a toolchain file -- otherwise Bazel execs a path that was never
# staged into the sandbox and reports the unhelpful
# "execvp(...musl_missing.sh): No such file or directory" instead of the message.
filegroup(
    name = "placeholder",
    srcs = ["musl_missing.sh"],
)

cc_toolchain(
    name = "{cc_toolchain_name}",
    toolchain_config = ":{config_name}",
    all_files = ":placeholder",
    compiler_files = ":placeholder",
    dwp_files = ":placeholder",
    linker_files = ":placeholder",
    objcopy_files = ":placeholder",
    strip_files = ":placeholder",
    supports_param_files = 0,
)

toolchain(
    name = "{toolchain_name}",
    exec_compatible_with = [],
    target_compatible_with = [
        "@platforms//os:linux",
        "{cpu_constraint}",
        "@edgeclaw//:libc_musl",
    ],
    toolchain = ":{cc_toolchain_name}",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(
        config_name = config_name,
        cc_toolchain_name = cc_toolchain_name,
        toolchain_name = toolchain_name,
        cpu_constraint = _MUSL_ARCHES[arch]["cpu_constraint"],
    ))

def _musl_toolchain_repo_impl(rctx):
    arch = rctx.attr.arch
    if arch not in _MUSL_ARCHES:
        fail("musl_toolchain_repo: unknown arch '{}', expected one of {}".format(
            arch,
            ", ".join(_MUSL_ARCHES.keys()),
        ))
    meta = _MUSL_ARCHES[arch]
    triple = meta["triple"]

    env_name, candidates = _candidate_prefixes(rctx, arch, meta)

    reasons = []
    chosen = ""
    for candidate in candidates:
        problem = _probe(rctx, candidate)
        if problem == None:
            chosen = candidate
            break
        reasons.append(problem)

    if not chosen:
        _write_missing(rctx, arch, env_name, reasons)
        return

    null_device = "NUL" if rctx.os.name.startswith("windows") else "/dev/null"
    result = rctx.execute(
        [chosen + "gcc", "-E", "-v", "-x", "c", null_device],
        environment = {"LC_ALL": "C"},
    )
    if result.return_code != 0:
        fail("musl_toolchain_repo: failed to query {}: {}".format(chosen + "gcc", result.stderr))

    include_dirs = _parse_builtin_includes(result.stderr)

    # Add the sysroot roots the include parse can miss when the toolchain is a
    # prefix outside /usr (musl-cross-make keeps headers under <prefix>/<triple>).
    for marker in ["/bin/", "/"]:
        if marker in chosen:
            base_dir = chosen[:chosen.rfind(marker)] + "/"
            include_dirs.extend(_get_sysroot_dirs(chosen, base_dir))
            break

    _write_found(rctx, arch, meta, chosen, include_dirs)

musl_toolchain_repo = repository_rule(
    implementation = _musl_toolchain_repo_impl,
    attrs = {
        "arch": attr.string(mandatory = True),
    },
    # Re-run when the pointer moves. The autodetected /opt/... paths are not listed
    # because they cannot change under a pinned checkout.
    environ = [
        "MUSL_CROSS_COMPILE_AARCH64",
        "MUSL_CROSS_COMPILE_ARM",
        "MUSL_CROSS_COMPILE_AMD64",
    ],
)
