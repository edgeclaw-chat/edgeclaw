load("@rules_pkg//pkg:mappings.bzl", "pkg_attributes", "pkg_files", "strip_prefix")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")

# Exported so //src/common:version_header can resolve a path inside the checkout. That is
# how tools/gen_version.py finds .git: the genrule runs in the execroot, where the tree is
# a set of symlinks, and realpath() of any tracked file lands back in the source tree.
exports_files(
    ["BUILD"],
    visibility = ["//visibility:public"],
)

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

# Build with `--define=valgrind=1` to drop `-static` and produce a dynamically
# linked binary, so valgrind can intercept malloc/free and track the heap.
config_setting(
    name = "valgrind",
    define_values = {"valgrind": "1"},
    visibility = ["//visibility:public"],
)

# `--define=sanitize=1` (set by --config=asan): ASAN needs a dynamic runtime, so every
# target that would otherwise link -static has to drop it. Kept separate from `valgrind`
# because the two are used for different runs and may one day want different link flags.
config_setting(
    name = "sanitize",
    define_values = {"sanitize": "1"},
    visibility = ["//visibility:public"],
)

platform(
    name = "arm_linux",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:armv7",
    ],
)

# ── musl: the fully static release binaries ───────────────────────────────────
# `--config=musl_<arch>` (see .bazelrc) switches to these. The libc constraint is
# what makes the choice unambiguous: the musl toolchains require `libc:musl`, and
# no glibc platform carries it, so a normal build never sees them. Keeping musl
# behind its own constraint instead of replacing the toolchain means the
# development loop, `--config=asan` and the tests all stay on glibc -- which they
# must, because a musl toolchain ships no libasan/libubsan.
constraint_setting(
    name = "libc",
    default_constraint_value = ":libc_glibc",
    visibility = ["//visibility:public"],
)

constraint_value(
    name = "libc_glibc",
    constraint_setting = ":libc",
    visibility = ["//visibility:public"],
)

# Public because the musl toolchains live in external repos and must name it in
# their target_compatible_with.
constraint_value(
    name = "libc_musl",
    constraint_setting = ":libc",
    visibility = ["//visibility:public"],
)

platform(
    name = "aarch64_linux_musl",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:aarch64",
        ":libc_musl",
    ],
)

platform(
    name = "arm_linux_musl",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:armv7",
        ":libc_musl",
    ],
)

platform(
    name = "amd64_linux_musl",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
        ":libc_musl",
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

# The musl platforms need their own entries in FILE_NAME / DAGENT_FILE_NAME below.
# These match on the literal --platforms string, so `//:aarch64_linux_musl` does not
# fall through to `is_aarch64_linux`; without an entry it used to land on
# "edgeclaw_unkown", which would have collided with any other unknown-platform build
# and given the release archive a name that does not say what it is.
config_setting(
    name = "is_aarch64_linux_musl",
    constraint_values = [],
    values = {"platforms": "//:aarch64_linux_musl"},
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
    name = "is_amd64_windows",
    constraint_values = [],
    values = {"platforms": "//:amd64_windows"},
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

config_setting(
    name = "is_arm_linux_musl",
    constraint_values = [],
    values = {"platforms": "//:arm_linux_musl"},
)

config_setting(
    name = "is_amd64_linux_musl",
    constraint_values = [],
    values = {"platforms": "//:amd64_linux_musl"},
)

FILE_NAME = select({
        ":is_aarch64_linux_musl": "edgeclaw_aarch64_linux_musl",
        ":is_arm_linux_musl": "edgeclaw_arm_linux_musl",
        ":is_amd64_linux_musl": "edgeclaw_amd64_linux_musl",
        ":is_arm_linux": "edgeclaw_arm_linux",
        ":is_aarch64_linux": "edgeclaw_aarch64_linux",
        ":is_amd64_linux": "edgeclaw_amd64_linux",
        ":is_aarch64_macos": "edgeclaw_aarch64_macos",
        ":is_amd64_windows": "edgeclaw_amd64_windows",
        # Fallback for a plain `bazel build //:edgeclaw` on Windows, where no
        # --platforms is passed and the value-based settings above cannot match.
        ":windows": "edgeclaw_amd64_windows",
        "//conditions:default": "edgeclaw_unkown",
})

DAGENT_FILE_NAME = select({
        ":is_aarch64_linux_musl": "device_agent_aarch64_linux_musl",
        ":is_arm_linux_musl": "device_agent_arm_linux_musl",
        ":is_amd64_linux_musl": "device_agent_amd64_linux_musl",
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
    srcs = ["//src/tui:edgeclaw"],
    attributes = pkg_attributes(mode = "0755"),
    renames = select({
        ":windows": {"//src/tui:edgeclaw": "edgeclaw.exe"},
        "//conditions:default": {"//src/tui:edgeclaw": "edgeclaw"},
    }),
)


WORKSPACE_FILES = glob(["workspace/**"])

# Default workspace tree, embedded into the binary at build time (see
# //src/core:workspace_embedded_c).
filegroup(
    name = "workspace_files",
    srcs = WORKSPACE_FILES,
    visibility = ["//visibility:public"],
)

# The user manual, embedded into the binary at build time and released to
# $EDC_HOME/docs on startup (see //src/common:common + edc_init_env_docs). The system
# prompt points the model at <edc_home>/docs/edgeclaw.md, so "how do I use EdgeClaw" is
# answerable from the binary alone -- no source tree, no docs/ copy to locate.
#
# An explicit list, NOT glob(["docs/**"]): docs/ holds two kinds of text. These are the
# product manual -- what a user, or the model acting for one, needs to OPERATE
# EdgeClaw. The rest are notes for people working ON it: they answer no user question,
# while every binary that carries them pays for them. A chapter added here ships; the
# pinned set and its complement are asserted in //tests:test_bundled_docs.
DOCS_FILES = [
    "docs/edgeclaw.1",                # the man page
    "docs/edgeclaw.md",               # the manual: commands, sessions, tasks, tools
    "docs/input-syntax.zh.md",        # quoting, the shell-like syntax, @file, !cmd
    "docs/limitations.md",            # what it does not do, and what is not verified
    "docs/local-shell-command.zh.md", # the !cmd local shell
    "docs/prompt-library.md",         # /prompt templates
    "docs/tool-output-limits.zh.md",  # truncation / output caps
]

filegroup(
    name = "docs_files",
    srcs = DOCS_FILES,
    visibility = ["//visibility:public"],
)

# Reuses the workspace embedder: same struct edc_bundled_file, same path-sorted output;
# only the symbol base name and the stripped prefix differ.
genrule(
    name = "docs_embedded_c",
    srcs = [":docs_files"],
    outs = ["docs_embedded.c"],
    cmd = "$(execpath //tools:embed_workspace) --out $@ --prefix docs --name edc_bundled_docs --header edc_bundled_docs.h $(SRCS)",
    cmd_bat = "$(execpath //tools:embed_workspace) --out $@ --prefix docs --name edc_bundled_docs --header edc_bundled_docs.h $(SRCS)",
    tools = ["//tools:embed_workspace"],
    visibility = ["//visibility:public"],
)

# The AI-model catalog (models.dev, via tools/gen_models.py). It is NOT committed (a
# generated build input; see .gitignore), so this rule produces it as part of the build:
# when a local models_catalog.json exists it is used as-is, otherwise the tool fetches it.
# `--no-test` keeps the fetch fast -- it skips per-provider endpoint probes (run
# tools/gen_models.py directly for the full validation).
#
# The local-file branch is not a convenience, it is what makes a release provably fresh:
# Release runs `bazel run //tools:gen_models -- --out models_catalog.json` before building,
# and because that file is one of this rule's srcs, changing it changes the ACTION KEY, so
# the genrule re-runs even when the persisted disk cache has an entry from a previous
# release. Without it the cache would happily ship last month's catalog.
#
# It also keeps an ordinary `bazel build` hermetic: a developer's tree has the file, so no
# network is touched, and two builds of the same commit embed the same bytes. That is the
# one place where determinism matters more than freshness -- a release gets freshness from
# the step that rewrites the file, which is why that step exists.
#
# --no-sandbox/local: the fetch branch needs network, which sandboxed Bazel actions do not
# have.
genrule(
    name = "models_catalog_json",
    srcs = glob(["models_catalog.json"], allow_empty = True),
    outs = ["models_catalog.generated.json"],
    # One tool invocation, no shell constructs: a checked-in models_catalog.json wins, otherwise
    # the tool fetches. The "if the override is there" decision used to be `[ -f ... ] && cp` in
    # the command, which is the one thing that cannot be mirrored into cmd_bat without writing
    # the same logic twice. It lives in the tool now, so both platforms run identical code.
    cmd = "$(execpath //tools:gen_models) --no-test --out $@ --copy-if-present models_catalog.json",
    cmd_bat = "$(execpath //tools:gen_models) --no-test --out $@ --copy-if-present models_catalog.json",
    tools = ["//tools:gen_models"],
    tags = ["no-sandbox", "requires-network"],
    local = 1,
    visibility = ["//visibility:public"],
)

# Embed the catalog as a byte array so the wizard / model picker work offline, with no
# file next to the executable. The input always comes from //:models_catalog_json, so
# the empty-catalog fallback in tools/embed_models.py is no longer reachable from here.
genrule(
    name = "models_catalog_embedded_c",
    srcs = [":models_catalog_json"],
    outs = ["models_catalog_embedded.c"],
    cmd = "$(execpath //tools:embed_models) --out $@ --name edc_bundled_models_catalog --header edc_bundled_models_catalog.h $(SRCS)",
    cmd_bat = "$(execpath //tools:embed_models) --out $@ --name edc_bundled_models_catalog --header edc_bundled_models_catalog.h $(SRCS)",
    tools = ["//tools:embed_models"],
    visibility = ["//visibility:public"],
)

# Licence texts travel WITH the binary. EdgeClaw ships as one statically linked executable,
# so distributing it distributes every library linked into it -- those licences have to
# accompany the archive, or the release is not compliant. Refreshed and checked by
# tools/gen_notices.py; the texts are committed rather than generated during the build so a
# release does not depend on a Bazel external cache happening to exist (see that script).
pkg_files(
    name = "licenses_files",
    srcs = [
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
    ] + glob(["licenses/**"]),
    # from_root(""), not the default: the default strips ALL directories, which would
    # flatten licenses/<component>/LICENSE onto a single colliding `LICENSE`.
    strip_prefix = strip_prefix.from_root(""),
    visibility = ["//visibility:public"],
)

# Release archive: a single self-contained binary. The default workspace is
# embedded in the binary and released to ~/.edgeclaw/ on first run, so no
# workspace/ copy ships alongside it.
pkg_tar(
    name = "edgeclaw",
    srcs = [
        ":edgeclaw_binary",
        ":licenses_files",
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
        ":licenses_files",
    ],
    extension = "tar.gz",
    package_dir = DAGENT_FILE_NAME,
    package_file_name = DAGENT_FILE_NAME + ".tar.gz",
)
