load("@rules_python//python:defs.bzl", "py_binary")

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
