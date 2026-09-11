load("@rules_python//python:defs.bzl", "py_binary")

# Build-time tool: turns the workspace/ tree into a C source file so it can be
# linked into the binary (see //src/core:workspace_embedded_c).
py_binary(
    name = "embed_workspace",
    srcs = ["embed_workspace.py"],
    visibility = ["//visibility:public"],
)
