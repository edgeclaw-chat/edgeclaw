# Shared cc_toolchain feature definitions that map --compilation_mode onto
# optimization flags.
#
# Why this file exists
# --------------------
# A cc_toolchain built with cc_common.create_cc_toolchain_config_info() only
# receives `-O` flags if its feature list contains:
#
#   1. the three *marker* features named exactly after the compilation modes
#      ("fastbuild", "dbg", "opt") -- Bazel itself enables the one matching
#      --compilation_mode (see rules_cc configure_features.bzl:
#      `all_features = [cpp_configuration.compilation_mode()]`), and
#   2. a feature whose flag_sets are guarded by
#      with_feature_set(features = ["opt"|"dbg"|"fastbuild"]).
#
# A toolchain that defines only tool_paths/action_configs therefore never gets
# an `-O` flag: every translation unit is compiled at the compiler default
# (-O0 for GCC/Clang), and `--compilation_mode=opt` silently has no effect on
# codegen. That is exactly what happened here -- the "aarch64-opt" and
# "aarch64-fastbuild" object files were byte-identical.
#
# The flag values mirror Bazel's own defaults (unix_cc_configure.bzl) minus the
# hardening/section flags this project deliberately opts out of
# (-D_FORTIFY_SOURCE, -ffunction-sections/-fdata-sections: the links use
# -Wl,--no-gc-sections, so per-section data would only bloat the static binary).
#
# NOTE: these are plain strings spliced into generated Starlark that is later
# run through str.format(), so they must stay free of `{` / `}`. They are
# substituted as format *values*, which is brace-safe, but keeping them
# brace-free avoids surprises if a caller ever inlines them.

# Local alias for the set of actions that consume compile flags. Defined once
# and referenced by the flag_sets below. Actions that the toolchain never
# configures are simply not applied.
COMPILATION_MODE_ACTIONS = """    all_compile_actions = [
        "c-compile",
        "c++-compile",
        "linkstamp-compile",
        "assemble",
        "preprocess-assemble",
        "c++-header-parsing",
        "c++-module-compile",
        "c++-module-codegen",
        "clif-match",
        "lto-backend",
    ]
"""

# Marker features + the actual compilation-mode -> optimization mapping.
COMPILATION_MODE_FEATURES = """        feature(name = "fastbuild"),
        feature(name = "dbg"),
        feature(name = "opt"),
        feature(
            name = "compilation_mode_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(flags = ["-O2", "-DNDEBUG"]),
                    ],
                    with_features = [with_feature_set(features = ["opt"])],
                ),
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(flags = ["-O0", "-g"]),
                    ],
                    with_features = [with_feature_set(features = ["dbg"])],
                ),
                flag_set(
                    actions = all_compile_actions,
                    flag_groups = [
                        flag_group(flags = ["-O0"]),
                    ],
                    with_features = [with_feature_set(features = ["fastbuild"])],
                ),
            ],
        ),
"""

# The symbols the generated config needs to load for the block above.
COMPILATION_MODE_LOAD_SYMBOLS = [
    "feature",
    "flag_set",
    "flag_group",
    "with_feature_set",
]
