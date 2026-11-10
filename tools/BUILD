load("@rules_python//python:defs.bzl", "py_binary")

# The installer users pipe into a shell. An sh_binary rather than a bare source file so
# `//tests:test_install_sh` finds it in its runfiles, and so
# `bazel run //tools:install -- --dry-run` works.
sh_binary(
    name = "install",
    srcs = ["install.sh"],
    visibility = ["//visibility:public"],
)

# Build-time tool: turns the workspace/ tree into a C source file so it can be
# linked into the binary (see //src/common:workspace_embedded_c).
py_binary(
    name = "embed_workspace",
    srcs = ["embed_workspace.py"],
    visibility = ["//visibility:public"],
)

# Build-time tool: embeds a single file (the sandbox helper executable) as a C
# byte array (see //src/sandbox:sandbox_helper_embedded_c).
py_binary(
    name = "embed_blob",
    srcs = ["embed_blob.py"],
    visibility = ["//visibility:public"],
)

# Build-time tool: fetches models.dev and writes models_catalog.json (see
# //:models_catalog_json). `--no-test` is used by the build so a slow/unreachable
# provider endpoint cannot stall it; run the script directly for the full probe.
py_binary(
    name = "gen_models",
    srcs = ["gen_models.py"],
    visibility = ["//visibility:public"],
)

# Build-time tool: embeds the generated AI-model catalog as a C byte array (see
# //:models_catalog_embedded_c). An absent catalog embeds an empty one.
py_binary(
    name = "embed_models",
    srcs = ["embed_models.py"],
    visibility = ["//visibility:public"],
)

# Build-time tool: turns `git describe` into the C header behind `--version` (see
# //src/common:version_header). Generated, not checked in: a hand-written version string
# drifts from the commit it claims to describe and nobody notices until a bug report
# names a version that was never released.
py_binary(
    name = "gen_version",
    srcs = ["gen_version.py"],
    visibility = ["//visibility:public"],
)

# ── the release-signature pipeline ──────────────────────────────────────────
#
# Three copies of one public key must stay in step: this PEM, the C array compiled into
# the binary, and the copy embedded in install.sh (which runs as `curl | sh` and has no
# checkout). Drift does not crash anything -- every update is simply rejected -- so the two
# tools that move the key between those copies are tested rather than trusted.

# PEM -> C array. Run by hand when rotating, and its output is committed.
py_binary(
    name = "gen_pubkey_c",
    srcs = ["gen_pubkey_c.py"],
    visibility = ["//visibility:public"],
)

# The PEM -> install.sh direction. `--check` reports whether the copies agree.
py_binary(
    name = "embed_pubkey_install",
    srcs = ["embed_pubkey_install.py"],
    visibility = ["//visibility:public"],
)

# Key generation and rotation, with the steps to update every copy printed at the end.
sh_binary(
    name = "update_keygen",
    srcs = ["update_keygen.sh"],
    visibility = ["//visibility:public"],
)

# The committed trust anchor. A plain file rule so //tests:test_update_keygen can read it
# from its runfiles.
exports_files(
    ["update_signing.pub.pem"],
    visibility = ["//visibility:public"],
)
