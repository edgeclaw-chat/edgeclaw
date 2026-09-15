load("@rules_cc//cc:defs.bzl", "cc_library")

# ── Top-level utilities ───────────────────────────────────────────────────

# ── Backward-compatibility aliases ────────────────────────────────────────
# 注: 聚合伞 `//src:agent`（agent + shell + core + device）已于 2026-09-15 删除 ——
# 它让测试可以静默越界; 现在每个测试只依赖自己真正用到的模块, 越界即编译失败。

alias(
    name = "edc_string_utils",
    actual = "//src/common:edc_string_utils",
    visibility = ["//visibility:public"],
)

alias(
    name = "mbedtls_config",
    actual = "//src/common:mbedtls_config",
    visibility = ["//visibility:public"],
)

alias(
    name = "device_agent",
    actual = "//src/device:device_agent",
    visibility = ["//visibility:public"],
)
