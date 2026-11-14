LINUX_LINK_OPTS = [
    "-lrt",
    "-lpthread",
    "-Wl,--no-gc-sections",
]

MACOS_LINK_OPTS = [
    "-lpthread",
    "-lresolv",
]

WINDOWS_LINK_OPTS = [
    # MSVC spellings; the GNU `-lfoo` forms arrive at link.exe as `/lfoo`, which it reports as
    # `warning LNK4044: unrecognized option` and then links nothing, so the symbols these
    # libraries provide fail at the end as LNK2019 instead. advapi32 comes from libuv's use of
    # the token API (OpenProcessToken in uv__getpwuid_r on Windows).
    "ws2_32.lib",
    "bcrypt.lib",
    "userenv.lib",
    "advapi32.lib",
]

LINK_OPTS = select({
    "//:macos": MACOS_LINK_OPTS,
    "//:linux": LINUX_LINK_OPTS,
    "//:windows": WINDOWS_LINK_OPTS,
    "//conditions:default": [], 
})

# C++ 测试（TUI 那 11 个）：这些 target 由 gcc 驱动链接，不会自动带上 C++ 运行库（原
# 在 src/tui/BUILD 里靠它自己的 LINK_OPTS 补 -lstdc++；搬过来后按同一手法补）。
TUI_TEST_LINK_OPTS = LINK_OPTS + select({
    # MSVC links its own STL; there is no -lstdc++ to add.
    "//:windows": [],
    "//conditions:default": ["-lstdc++"],
})

# 用 openpty() 的测试。macOS 把它放在 libSystem 里（不需要额外的库，声明在 <util.h>），
# Linux 放在单独的 libutil 里（声明在 <pty.h>），所以库这一半要按平台给；头文件那一半在
# 各个 .c 里按 __APPLE__ 选。
PTY_LINK_OPTS = LINK_OPTS + select({
    "//:linux": ["-lutil"],
    "//conditions:default": [],
})

# Tests whose SUBJECT does not exist on Windows, expressed as "compatible everywhere except
# Windows" so `bazel test //tests:all` is honest there instead of red:
#
#   - pseudo-terminals, termios and the openpty()-driven pppd transport
#   - fork()/waitpid() process supervision and the POSIX shell
#   - pthreads (Windows has threads, but not this API)
#   - <arpa/inet.h>-level socket programming in the TLS stub
#
# Shimming these would produce tests that compile and assert nothing, which is worse than
# saying plainly that the platform lacks the feature. The product already reports
# kernel_sandbox = false on Windows for the same kind of reason.
#
# @platforms//os:none is a constraint no platform satisfies, so the target is simply not
# built; on Linux and macOS the select yields [] and nothing changes.
POSIX_ONLY = select({
    "//:windows": ["@platforms//os:none"],
    "//conditions:default": [],
})

cc_test(
    name = "test_containers",
    srcs = ["test_containers.c", "test_ws.h"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
)

# The wall-clock helpers: the millisecond clock that feeds message ids, and the
# format/parse pair every timestamp in a log line goes through. Asserted as
# round trips, so the test does not encode the build machine's timezone.
cc_test(
    name = "test_datetime",
    srcs = ["test_datetime.c"],
    deps = [
        "//src/common:edc_datetime_utils",
    ],
    linkopts = LINK_OPTS,
)

# 活体手测（打真 API + 要 EDGECLAW_MASTER_KEY），不当自动化跑；手动：
# EDGECLAW_MASTER_KEY=... bazel test //tests:test_llm_actor --test_output=all
cc_test(
    name = "test_llm_actor",
    srcs = ["test_llm_actor.c", "test_ws.h"],
    tags = ["manual"],
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_lwip_http",
    srcs = ["test_lwip_http.c", "test_ws.h"],
    deps = [
        "//src/device:device",
        "//thirdparty/lwip:sys_arch",
        "@lwip//:lwip",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_pppos_http",
    srcs = ["test_pppos_http.c", "test_ws.h"],
    deps = [
        "//thirdparty/lwip:sys_arch",
        "@lwip//:lwip",
    ],
    linkopts = PTY_LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

# pppos 压力测试：宿主机上会不定期跑满 300s 超时（flaky），不纳入默认测试集。
# 仍可显式跑：bazel test //tests:test_pppos_stress。
cc_test(
    name = "test_pppos_stress",
    srcs = ["test_pppos_stress.c", "test_ws.h"],
    tags = ["manual"],
    deps = [
        "//thirdparty/lwip:sys_arch",
        "@lwip//:lwip",
    ],
    linkopts = LINUX_LINK_OPTS + ["-lutil"],
)

cc_test(
    name = "test_pppos_agent",
    srcs = ["test_pppos_agent.c", "test_ws.h"],
    deps = [
        "//src/device:device",
        "//thirdparty/lwip:sys_arch",
        "@lwip//:lwip",
    ],
    linkopts = PTY_LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

# 宿主机上固定失败（`echo $HOME` 断言与环境白名单不符），排除出默认测试集。
# 仍可显式跑：bazel test //tests:test_pppos_agent_stress。
cc_test(
    name = "test_pppos_agent_stress",
    srcs = ["test_pppos_agent_stress.c", "test_ws.h"],
    tags = ["manual"],
    deps = [
        "//src/common:common",
        "//src/device:device",
        "//thirdparty/lwip:sys_arch",
        "@cjson//:cjson",
        "@lwip//:lwip",
    ],
    linkopts = LINUX_LINK_OPTS + ["-lutil"],
)

cc_binary(
    name = "pppos_cli",
    srcs = ["pppos_cli.c"],
    deps = [
        "//src/device:device",
        "//thirdparty/lwip:sys_arch",
        "@cjson//:cjson",
        "@lwip//:lwip",
    ],
    linkopts = LINK_OPTS,
    # termios + the pty-driven pppd transport: does not exist on Windows.
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_strutil",
    srcs = ["test_strutil.c", "test_ws.h"],
    deps = [
        "//src:edc_string_utils",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_redact",
    srcs = ["test_redact.c"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_upgrade",
    srcs = ["test_upgrade.c"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_update_check",
    srcs = ["test_update_check.c", "tls_stub.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "@mbedtls//:mbedtls",
        "@libuv",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_update",
    srcs = ["test_update.c"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

sh_test(
    name = "test_embed_workspace",
    srcs = ["test_embed_workspace.sh"],
    data = ["//tools:embed_workspace"],
    target_compatible_with = POSIX_ONLY,
)

sh_test(
    name = "test_genrule_portability",
    srcs = ["test_genrule_portability.sh"],
    data = ["//tools:check_genrule_portability"],
    target_compatible_with = POSIX_ONLY,
)

sh_test(
    name = "test_portability",
    srcs = ["test_portability.sh"],
    data = ["//tools:check_portability"],
    target_compatible_with = POSIX_ONLY,
)
cc_test(
    name = "test_semver",
    srcs = ["test_semver.c"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

# The --help block, cross-checked against main.cpp's option parser: every long option the
# binary answers must be documented. That is the property that was missing when
# --update-check shipped without a line in --help.
cc_test(
    name = "test_version",
    srcs = ["test_version.c", "test_fs_util.h"],
    data = ["//src/tui:main_cpp"],
    deps = [
        "//src/common:version",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_https_redirect",
    srcs = ["test_https_redirect.c"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_https_client_get",
    srcs = ["test_https_client_get.c", "test_ws.h"],
    deps = [
        "//src/common:common",
    ],
    # Live network: two real TLS round trips to https://example.com (one 200, one 404 --
    # the second exists so a hardcoded status cannot pass). Kept out of the default set so
    # "121 green" does not silently require an internet connection. To bring it back
    # offline, extract the mbedtls server from tests/test_endpoint_probe.c into a shared
    # test header and point this at 127.0.0.1, the way test_endpoint_probe already does.
    tags = ["manual"],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_group_ctx",
    srcs = ["test_group_ctx.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_tools_memory",
    srcs = ["test_tools_memory.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@log.c//:log.c",
        "@uthash//:uthash",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_memory",
    srcs = ["test_session_memory.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_context_estimate",
    srcs = ["test_context_estimate.c", "test_ws.h"],
    deps = [
        "//src/kernel:kernel",
        "@cjson//:cjson",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_task",
    srcs = ["test_edc_task.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_task_group",
    srcs = ["test_edc_task_group.c", "test_ws.h"],
    deps = [
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_task_stress",
    srcs = ["test_edc_task_stress.c", "test_ws.h"],
    deps = [
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_scheduler",
    srcs = ["test_edc_scheduler.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_toolset",
    srcs = ["test_toolset.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/toolbox:toolbox",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_output_buf",
    srcs = ["test_output_buf.c", "test_ws.h"],
    deps = [
        "//src/toolbox:toolbox",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_toolbox_spill",
    srcs = ["test_toolbox_spill.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_tools_fs",
    srcs = ["test_tools_fs.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_tool_shell_loop",
    srcs = ["test_tool_shell_loop.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

# tools/install.sh: which tag is current, which asset this machine needs, and what a failed
# download is allowed to leave behind. Offline -- `curl` and `uname` are shimmed -- because
# the interesting cases are the refusals, and a test that needs GitHub to answer is a test
# that fails for reasons that have nothing to do with this script.
# test_install_ps1 serves its fixtures over loopback and drives pwsh, so it cannot run inside
# the network namespace the sandbox creates -- loopback is unreachable there, which showed up
# as "download failed" with a perfectly correct URL. `local = True` runs it outside the
# sandbox; nothing it touches is shared with the other tests (private temp dir, a port it
# picks itself).
sh_test(
    name = "test_install_ps1",
    srcs = ["test_install_ps1.sh"],
    data = ["//tools:install.ps1"],
    local = True,
    target_compatible_with = POSIX_ONLY,
)

# Runs test_install_ps1 only where pwsh exists: the script skips itself otherwise, and a
# local run without pwsh proves nothing about the PowerShell installer.
sh_test(
    name = "test_install_sh",
    srcs = ["test_install_sh.sh"],
    data = ["//tools:install"],
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_tools_search_loop",
    srcs = ["test_tools_search_loop.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

# grep / find 语义矩阵：跳过清单、深度上限、二进制与大小闸门、CRLF、行截断、limit、
# literal vs regex、glob 表，外加多线程并发走树（走的是 edc_search_run_blocking，不需要
# 起 actor loop；工具层那半截在 test_tools_search_loop 里）。
cc_test(
    name = "test_search_matrix",
    srcs = ["test_search_matrix.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/toolbox:toolbox",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_local_shell",
    srcs = ["test_local_shell.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/shell:shell",
        "//src/toolbox:toolbox",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_edc_session_stress",
    srcs = ["test_edc_session_stress.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_group_level",
    srcs = ["test_group_level.c", "test_ws.h"],
    deps = [
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_help",
    srcs = ["test_commands_help.c", "test_ws.h"],
    deps = [
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_receipt",
    srcs = ["test_commands_receipt.c", "test_ws.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_task_new",
    srcs = ["test_commands_task_new.c", "test_ws.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_sandbox_paths",
    srcs = ["test_sandbox_paths.c", "test_fs_util.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/sandbox:sandbox",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_file_index",
    srcs = ["test_file_index.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:edc_file_utils",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_mention_pass",
    srcs = ["test_mention_pass.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_task_fork",
    srcs = ["test_commands_task_fork.c", "test_ws.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_shell_job_control",
    srcs = ["test_shell_job_control.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_shell_switch",
    srcs = ["test_shell_switch.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

# The shell input syntax as a SPEC: every row is a sentence from
# docs/input-syntax.zh.md, driven through the real edc_lex -> edc_classify ->
# edc_parse pipeline and asserted on the AST. It exists because one class of bug
# has no natural home in a feature test -- a lexer rule that disagrees with the
# documented quoting (a """...""" body holding a bare quote was split into three
# words, which the grammar then refused mid-line). A matrix over
# (quote form x payload that collides with the delimiter) fails on any cell.
cc_test(
    name = "test_shell_spec",
    srcs = ["test_shell_spec.c", "test_ws.h"],
    deps = [
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_session_memo",
    srcs = ["test_commands_session_memo.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "//src/shell:shell",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_report",
    srcs = ["test_commands_report.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "//src/shell:shell",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_session_remove",
    srcs = ["test_commands_session_remove.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "//src/shell:shell",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_state_probe",
    srcs = ["test_state_probe.c", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_commands_session_new",
    srcs = ["test_commands_session_new.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "//src/shell:shell",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_group_fold",
    srcs = ["test_commands_group_fold.c", "test_ws.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

# End-to-end cost through the public door (edc_session_save_state/load_state). Manual:
#   EDGECLAW_STATE_PROBE=/tmp/probe/shell.csv \
#     bazel run //tests:bench_shell -- <workspace> 40000 3
cc_binary(
    name = "bench_shell",
    srcs = ["bench_shell.c", "test_ws.h", "test_fs_util.h"],
    tags = ["manual"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

# Stage-1 cost ruler for the store (full rewrite on every flush). Manual:
#   EDGECLAW_STATE_PROBE=/tmp/probe/store.csv \
#     bazel run //tests:bench_store -- <workspace> 40000 3
cc_binary(
    name = "bench_store",
    srcs = ["bench_store.c", "test_ws.h", "test_fs_util.h"],
    tags = ["manual"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

# STAGE 1 GATE: the SQLite round trip must preserve every field the serializer writes.
cc_test(
    name = "test_state_store_differential",
    srcs = ["test_state_store_differential.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
        "@sqlite3//:sqlite3",
    ],
    linkopts = LINK_OPTS,
)

# Per-message identity: assigned once, kept on reload, and never sent to the model.
cc_test(
    name = "test_message_id",
    srcs = ["test_message_id.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

# The startup contract of the SQLite state store: PRAGMAs, schema, version refusal,
# symlink refusal, exclusive workspace lock, checkpoint on close.
cc_test(
    name = "test_state_store_open",
    srcs = ["test_state_store_open.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
        "@sds//:sds",
        "@sqlite3//:sqlite3",
    ],
    linkopts = LINK_OPTS,
)

# The schema-upgrade runner (edc_state_migrate.c). Drives the runner with synthetic steps
# -- success, failure before a write, failure AFTER a write, a verify that fails, a hole in
# the chain -- and then exercises the real v7 -> v8 step against a hand-built v7 database.
# The properties it pins (backup readable and pre-migration, DDL rolled back, chain checked
# before any write, newer schema refused) are the ones whose absence loses a user's history.
cc_test(
    name = "test_state_migrate",
    srcs = ["test_state_migrate.c", "test_fs_util.h"],
    deps = [
        "//src/kernel:kernel",
        "@sds//:sds",
        "@sqlite3//:sqlite3",
    ],
    linkopts = LINK_OPTS,
)

# Guards the no-patch SQLite configuration: asserts that the compile options injected via
# .bazelrc --per_file_copt actually reached the library. Without this, a --per_file_copt
# rule that stops matching would silently yield an unhardened sqlite (see the test file).
cc_test(
    name = "test_sqlite_hardening",
    srcs = ["test_sqlite_hardening.c", "test_ws.h"],
    deps = [
        "@sqlite3//:sqlite3",
    ],
    linkopts = LINK_OPTS,
)

# Stage-0 baseline ruler for the SQLite migration.
# Needs a workspace with real session data, so it is a manual binary, not a test:
#
#   EDGECLAW_STATE_PROBE=/tmp/probe/s13.csv \
#     bazel run //tests:bench_state_io -- <workspace> <session_id> <iterations>
cc_binary(
    name = "bench_state_io",
    srcs = ["bench_state_io.c", "test_ws.h"],
    tags = ["manual"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_system_task",
    srcs = ["test_system_task.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_fold_files",
    srcs = ["test_fold_files.c"],
    deps = [
        "//src/kernel:kernel",
    ],
    linkopts = LINK_OPTS,
)

# The JOIN between edc_fold_files and the store -- the layer that was broken in both
# directions while each side's own tests were green.
cc_test(
    name = "test_fold_files_persist",
    srcs = ["test_fold_files_persist.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_group_fold",
    srcs = ["test_group_fold.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_snapshot",
    srcs = ["test_session_snapshot.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_routing",
    srcs = ["test_session_routing.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_snapshot_addressing",
    srcs = ["test_snapshot_addressing.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

# test_session_state_file was deleted with the session.json file it tested (tmp+rename,
# schema-version ranges, file permissions). Its surviving contracts are covered elsewhere:
#   "a new session loads as nothing"        -> test_state_store_differential (absent)
#   "write then read back"                  -> test_state_store_differential (roundtrip)
#   "overwrite does not corrupt"            -> test_state_store_differential (idempotent)
#   "corrupt/unknown state is refused"      -> test_state_store_open (version, quick_check)
#   "atomic replace"                        -> a transaction; nothing to test at file level

cc_test(
    name = "test_scheduler_open_close",
    srcs = ["test_scheduler_open_close.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
        "@sds//:sds",
        "@sqlite3//:sqlite3",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_path_absolute",
    srcs = ["test_path_absolute.c"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_sandbox_switch",
    srcs = ["test_sandbox_switch.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/common:edc_file_utils",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)
cc_test(
    name = "test_sandbox_verify_path",
    srcs = ["test_sandbox_verify_path.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/sandbox:sandbox",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)
# What the platform shims must DO, not merely that they compile. Each case pins a difference
# that was found on Windows and was invisible on POSIX: unsetenv() not reaching getenv(),
# realpath() resolving paths that do not exist, mkdir() without the POSIX two-argument form,
# and lstat()/S_ISLNK() being unable to see a link at all (which disarmed the state store's
# refuse_symlink()). Keep the assertions phrased as the caller's contract so a shim that
# drifts on either platform fails here.
cc_test(
    name = "test_compat_shims",
    srcs = ["test_compat_shims.c", "test_fs_util.h"],
    deps = [
        "//src/common:edc_compat",
        "//src/common:edc_file_utils",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_path_contract",
    srcs = ["test_path_contract.c", "test_ws.h"],
    deps = [
        "//src/common:edc_compat",
        "//src/common:edc_file_utils",
        "@libuv",
    ],
    linkopts = LINK_OPTS,
)
cc_test(
    name = "test_platform_caps",
    srcs = ["test_platform_caps.c", "test_ws.h"],
    deps = [
        "//src/common:edc_platform",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_utf8_valid",
    srcs = ["test_utf8_valid.c", "test_ws.h"],
    deps = [
        "//src/common:edc_compat",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_output_utf8",
    srcs = ["test_output_utf8.c", "test_ws.h"],
    deps = [
        "//src/common:edc_compat",
        "//src/common:edc_platform",
    ],
    linkopts = LINK_OPTS,
)
cc_test(
    name = "test_session_workdir",
    srcs = ["test_session_workdir.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/sandbox:sandbox",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_status",
    srcs = ["test_session_status.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_turn_history_integration",
    srcs = ["test_turn_history_integration.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_lifecycle_integration",
    srcs = ["test_lifecycle_integration.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_toolbox_response",
    srcs = ["test_toolbox_response.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_llm_request",
    srcs = ["test_llm_request.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/actor:llm_request",
        "//src/common:common",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_toolbox_request",
    srcs = ["test_toolbox_request.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_todo_sysprompt",
    srcs = ["test_todo_sysprompt.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_cmd_parse",
    srcs = ["test_cmd_parse.c", "test_ws.h"],
    deps = [
        "//src/shell:shell_parse",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_leave_cancel",
    srcs = ["test_session_leave_cancel.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_reactivate",
    srcs = ["test_session_reactivate.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_cross_dep",
    srcs = ["test_session_cross_dep.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_cross_dep_restart",
    srcs = ["test_cross_dep_restart.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_task_index",
    srcs = ["test_task_index.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_eviction",
    srcs = ["test_session_eviction.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_state_guardrail",
    srcs = ["test_state_guardrail.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_id",
    srcs = ["test_session_id.c", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

# 一个 flush 报出的 I/O 成本（`edc_state_store_flush_measured` 的非 NULL out 分支）。
# 其他测试都用 `edc_state_store_flush`（out = NULL），所以测量那一半此前从未执行。
cc_test(
    name = "test_flush_cost",
    srcs = ["test_flush_cost.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@cjson",
        "@log.c//:log.c",
        "@sds//:sds",
        "@uthash//:uthash",
    ],
    linkopts = LINK_OPTS,
)
cc_test(
    name = "test_task_resume",
    srcs = ["test_task_resume.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_bundled_workspace",
    srcs = ["test_bundled_workspace.c", "test_ws.h"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
)

# The bundled user manual: the table must be non-empty (the binary carries docs/), and
# edc_init_env must release it to <edc_home>/docs and refresh a stale copy -- the path
# the system prompt names.
cc_test(
    name = "test_bundled_docs",
    srcs = ["test_bundled_docs.c", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

# context_window 的配置面（默认 0 = 未配置/未知）：三个生产者都跑真的
# —— edc_init_env 建/补 config.json + edc_config_load 读回。
cc_test(
    name = "test_config_window",
    srcs = ["test_config_window.c", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@cjson//:cjson",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_config_onboarded",
    srcs = ["test_config_onboarded.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_config_shell_syntax",
    srcs = ["test_config_shell_syntax.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_sandbox_helper",
    srcs = ["test_sandbox_helper.c", "test_ws.h", "test_fs_util.h"],
    # Must run OUTSIDE bazel's own sandbox: on macOS the test asserts that the
    # helper applies a Seatbelt profile, and sandbox_init() refuses to nest --
    # under darwin-sandbox it fails with "Operation not permitted" and the helper
    # (correctly) refuses to run the command.
    tags = ["local"],
    deps = [
        "//src/common:common",
        "//src/sandbox:sandbox",
        "@libuv",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_config_sandbox_defaults",
    srcs = ["test_config_sandbox_defaults.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/sandbox:sandbox",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_shell_env",
    srcs = ["test_shell_env.c", "test_ws.h"],
    deps = [
        "//src/sandbox:sandbox",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_secret",
    srcs = ["test_secret.c", "test_fs_util.h"],
    deps = [
        "//src/common:common",
    ],
    # openpty() drives the passphrase prompt through a real terminal.
    linkopts = PTY_LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_endpoint_probe",
    srcs = ["test_endpoint_probe.c", "test_ws.h", "tls_stub.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "@libuv",
        "@mbedtls//:mbedtls",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_connect",
    srcs = ["test_connect.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "@libuv",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_llm_actor_http_status",
    srcs = ["test_llm_actor_http_status.c", "test_ws.h"],
    tags = ["manual"],  # 网络 + 真 provider（故意用无效 key 拿 401），同 test_llm_actor
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/common:common",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_llm_actor_config",
    srcs = ["test_llm_actor_config.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/common:common",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

# What the actor does with the provider's ANSWER -- the failure paths. The parser is static
# and reached only when a real HTTPS response lands, so this drives it through a loopback TLS
# stub (tests/tls_stub.h) with a per-run certificate: real client, real status line, real
# parser. Before this file edc_llm_actor.c had 33.5% BRANCH coverage; the code that decides
# "context overflow" vs "bad key" had no test at all.
cc_test(
    name = "test_llm_actor_response",
    srcs = ["test_llm_actor_response.c", "tls_stub.h", "test_fs_util.h"],
    deps = [
        "//src/actor:actor",
        "//src/actor:actors",
        "//src/common:common",
        "//src/kernel:kernel",
        "@cjson//:cjson",
        "@libuv",
        "@log.c//:log.c",
        "@sds//:sds",
        "@uthash//:uthash",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

# The HTTPS transport when the transport is the problem: connection refused, a handshake
# against a plain-text server, the total deadline, and an unparseable URL. Uses the same
# loopback TLS stub as the LLM response test, plus a plain-TCP server for the handshake case
# (TLS is decided by mbedtls, not by the URL scheme).
cc_test(
    name = "test_https_client_failures",
    srcs = ["test_https_client_failures.c", "tls_stub.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "@libuv",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

# ── TUI 测试（原 src/tui/test_*.cpp，2026-09-15 归拢到 tests/）──────────

cc_test(
    name = "test_channel_dispatcher",
    srcs = ["test_channel_dispatcher.cpp"],
    deps = [
        "//src/tui:tui",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_command_palette",
    srcs = ["test_command_palette.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
)
cc_test(
    name = "test_status_bar_view",
    srcs = ["test_status_bar_view.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_domain_policies",
    srcs = ["test_domain_policies.cpp"],
    deps = [
        "//src/tui:tui",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_snapshot_claim",
    srcs = ["test_snapshot_claim.cpp"],
    deps = [
        "//src/tui:tui",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_task_snapshot",
    srcs = ["test_task_snapshot.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_tui_dispatch",
    srcs = [
        "test_tui_dispatch.cpp",
        "tui_dispatch_msgs.c",
        "tui_dispatch_msgs.h",
    ],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
        "@sds//:sds",
    ] + select({
        "//:windows": [],
        "//conditions:default": [":fake_release"],
    }),
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    # 这个 harness 会真起一个 app（临时 HOME + 最小 config），比纯渲染测试慢。
    timeout = "moderate",
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_theme",
    srcs = ["test_theme.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_scroll_regression",
    srcs = ["test_scroll_regression.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:component",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_text_wrap",
    srcs = ["test_text_wrap.cpp"],
    deps = [
        "//src/tui:tui",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

# The markdown renderer, driven through its single public entry (RenderMarkdown). Its block
# builders are file-local and reached only by md4c callbacks, so the only honest way to test
# them is to render markdown and read the drawn cells -- which is also the path a bug would
# take to a user. Before this file existed, markdown.cpp had 39.4% line coverage.
cc_test(
    name = "test_markdown",
    srcs = ["test_markdown.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_update_cli",
    srcs = ["test_update_cli.c", "test_fs_util.h", "fake_release.h"],
    deps = [
        ":fake_release",
        "//src/common:update_cli",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_update_service",
    srcs = ["test_update_service.c", "tls_stub.h", "test_fs_util.h"],
    deps = [
        "//src/kernel:kernel",
        "@libuv",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_upgrade_run",
    srcs = ["test_upgrade_run.c", "tls_stub.h", "test_fs_util.h"],
    deps = [
        "//src/common:update_run",
        "@libuv",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)

cc_test(
    name = "test_update_dialog",
    srcs = ["test_update_dialog.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
    target_compatible_with = POSIX_ONLY,
)

# Shared test helper: a one-shot local TLS server that answers the release endpoint with a
# given JSON. A cc_library rather than a header, so the .c is compiled as C for the C++ tests
# too (tls_stub.h is header-only C and does not survive a C++ include).
cc_library(
    name = "fake_release",
    testonly = True,
    srcs = ["fake_release.c"],
    hdrs = [
        "fake_release.h",
        "test_fs_util.h",
        "tls_stub.h",
    ],
    # Static on purpose: linking this as a shared library makes the TEST BINARIES dynamic,
    # and test_app_boundary's bootstrap copies its own executable into ~/.local/bin -- a copy
    # of a dynamic binary cannot resolve $ORIGIN-relative libraries from its new location.
    linkstatic = True,
    # POSIX only: it drives tls_stub.h, which includes <arpa/inet.h>. The tests that use it
    # guard their fake-release cases with #ifndef _WIN32 and depend on it through select().
    target_compatible_with = POSIX_ONLY,
    includes = ["."],
    deps = [
        "//src/common:common",
        "@libuv",
        "@mbedtls//:mbedtls",
        "@sds//:sds",
    ],
)

cc_test(
    name = "test_setup_dialog",
    srcs = ["test_setup_dialog.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
# 前端的 C 面（`//src/app:api`）+ 装配根（`//src/app:app`），经真 app 驱动。
# `struct edc_app` 不透明，所以每个收 app 的函数都必须有一个真 app —— 本文件用临时 HOME +
# 最小 config 起一个（与 test_tui_dispatch 同一套路），并且只起一次（建/拆都慢）。
cc_test(
    name = "test_app_boundary",
    srcs = ["test_app_boundary.c", "test_fs_util.h"],
    deps = [
        "//src/app:api",
        "//src/app:app",
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
        "@sds//:sds",
    ] + select({
        # POSIX only: it drives tls_stub.h. The fake-release cases are guarded by _WIN32.
        "//:windows": [],
        "//conditions:default": [":fake_release"],
    }),
    linkopts = LINK_OPTS,
    # 真起 app（scheduler + sqlite + 3 个 actor 线程）+ 一次 endpoint 探测。
    timeout = "moderate",
)
cc_test(
    name = "test_token_style",
    srcs = ["test_token_style.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkopts = TUI_TEST_LINK_OPTS,
)

cc_test(
    name = "test_history_line",
    srcs = ["test_history_line.cpp"],
    deps = [
        "//src/tui:tui",
    ],
    linkopts = TUI_TEST_LINK_OPTS,
)

cc_test(
    name = "test_edc_payload",
    srcs = ["test_edc_payload.cpp"],
    deps = [
        "//src/common:common",
        "//src/shell:shell",
    ],
    linkopts = TUI_TEST_LINK_OPTS,
)

cc_test(
    name = "test_edc_input",
    srcs = ["test_edc_input.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:component",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_working_status",
    srcs = ["test_working_status.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_chat_transcript",
    srcs = ["test_chat_transcript.cpp"],
    deps = [
        "//src/tui:tui",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
)
cc_test(
    name = "test_clipboard",
    srcs = ["test_clipboard.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_todo_widget",
    srcs = ["test_todo_widget.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_task_tree_model",
    srcs = ["test_task_tree_model.cpp"],
    deps = [
        "//src/tui:tui",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "test_task_widget",
    srcs = ["test_task_widget.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_chat_filter",
    srcs = ["test_chat_filter.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:component",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)
cc_test(
    name = "test_message_list",
    srcs = ["test_message_list.cpp"],
    deps = [
        "//src/tui:tui",
        "@ftxui//:component",
        "@ftxui//:dom",
        "@ftxui//:screen",
    ],
    linkstatic = True,
    linkopts = TUI_TEST_LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
)

cc_test(
    name = "test_llm_error_kind",
    srcs = ["test_llm_error_kind.c"],
    deps = [
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_executor_overflow",
    srcs = ["test_executor_overflow.c", "test_ws.h"],
    deps = [
        "//src/actor:actor",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_prompts",
    srcs = ["test_prompts.c", "test_fs_util.h"],
    deps = [
        "//src/shell:lex",
        "//src/kernel:kernel",
        "@sds//:sds",
        "@uthash//:uthash",
    ],
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)
cc_test(
    name = "test_commands_reload",
    srcs = ["test_commands_reload.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/kernel:kernel",
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)
cc_test(
    name = "test_resources",
    srcs = ["test_resources.c", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@sds//:sds",
        "@uthash//:uthash",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_jobs",
    srcs = ["test_commands_jobs.c", "test_ws.h"],
    deps = [
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_commands_prompt",
    srcs = ["test_commands_prompt.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/shell:shell",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_session_memory_extract",
    srcs = ["test_session_memory_extract.c", "test_ws.h"],
    deps = [
        "//src/actor:actors",
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_service",
    srcs = ["test_service.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/common:utlist_fix",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_models_catalog",
    srcs = ["test_models_catalog.c"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "@log.c//:log.c",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_crontab_tool",
    srcs = ["test_crontab_tool.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_crontab_service",
    srcs = ["test_crontab_service.c", "test_ws.h", "test_fs_util.h"],
    deps = [
        "//src/common:common",
        "//src/kernel:kernel",
        "//src/toolbox:toolbox",
        "@cjson//:cjson",
        "@log.c//:log.c",
        "@sds//:sds",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_parse_cmd",
    srcs = ["test_parse_cmd.c", "test_ws.h"],
    deps = [
        "//src/common:common",
        "//src/shell:lex",
        "//src/shell:parse",
    ],
    linkopts = LINK_OPTS,
)

cc_test(
    name = "test_lex",
    srcs = ["test_lex.c", "test_ws.h"],
    deps = [
        "//src/shell:lex",
    ],
    linkopts = LINK_OPTS,
)

# HTTPS through an HTTP CONNECT proxy: the environment decision, and the tunnel itself.
cc_test(
    name = "test_proxy",
    srcs = ["test_proxy.c", "tls_stub.h"],
    deps = [
        "//src/common:common",
        "@libuv",
        "@sds//:sds",
    ],
    linkstatic = True,
    linkopts = LINK_OPTS,
    target_compatible_with = POSIX_ONLY,
)
