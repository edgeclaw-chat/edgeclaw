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
