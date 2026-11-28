load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

LINK_OPTS = select({
    "//:macos_debug": ["-lresolv", "-lc++"],
    "//:macos": ["-lresolv", "-lc++"],
    # `-static` on MinGW, for the same reason the Linux default branch has it: without
    # it gcc links libstdc++-6.dll and libgcc_s_seh-1.dll dynamically, and pkg_tar
    # ships only edgeclaw.exe -- so the archive aborts on a clean machine with "The
    # code execution cannot proceed because libstdc++-6.dll was not found". The
    # ws2_32/bcrypt/userenv entries are import libraries for DLLs that are part of
    # Windows, so they stay dynamic (and -static does not turn them into anything else).
    # Verified by the "Verify Windows DLL imports" step in release.yml.
    # MSVC spellings. There is no `-static` for link.exe (a static CRT is a compile-time
    # choice, selected by the `static_link_msvcrt` feature in .bazelrc) and no `-lstdc++`
    # (MSVC links its own STL). The GNU `-lfoo` forms reached link.exe as `/lfoo`, which it
    # does not recognise -- it warns `LNK4044` and links nothing, so the symbols these
    # libraries provide surfaced as LNK2019 at the end instead.
    "//:windows": ["ws2_32.lib", "bcrypt.lib", "userenv.lib"],
    # `--define=valgrind=1`: dynamic link so valgrind can intercept malloc.
    "//:valgrind": ["-lstdc++"],
    # `--config=asan`: same reason -- ASAN needs a dynamic runtime.
    "//:sanitize": ["-lstdc++"],
    "//conditions:default": ["-static", "-Wl,--no-gc-sections", "-lstdc++"],
})

cc_library(
    name = "tui",
    srcs = [
        "edc_app_facade.cpp",
        "edc_channel_dispatcher.cpp",
        "edc_chat_bubble_page.cpp",
        "edc_chat_model.cpp",
        "edc_chat_transcript.cpp",
        "edc_chat_widget.cpp",
        "edc_chat_window.cpp",
        "edc_clipboard.cpp",
        "edc_command_palette.cpp",
        "edc_colorscheme_palette.cpp",
        "edc_setup_dialog.cpp",
        "edc_update_dialog.cpp",
        "edc_domain.cpp",
        "edc_history_line.cpp",
        "edc_input.cpp",
        "edc_message.cpp",
        "edc_modal.cpp",
        "edc_message_list.cpp",
        "edc_ports.cpp",
        "edc_theme.cpp",
        "edc_token_style.cpp",
        "edc_tui_window.cpp",
        "edc_scroll_view.cpp",
        "edc_sidebar.cpp",
        "edc_status_bar_view.cpp",
        "edc_task_snapshot.cpp",
        "edc_task_tree_model.cpp",
        "edc_task_widget.cpp",
        "edc_todo_widget.cpp",
        "edc_widget.cpp",
        "edc_working_indicator_view.cpp",
        "markdown.cpp",
        # Syntax engine compiled directly into this archive (not a separate
        # libsyntax.a) so its symbols and markdown.o/edc_message.o references
        # live in ONE archive: ld rescans archive members until symbols close,
        # which avoids the one-pass ordering problem with supports_start_end_lib
        # disabled.
        "//src/common:edc_syntax.c",
        "//src/common:embedded_queries",
    ],

    hdrs = [
        "edc_app_facade.hpp",
        "edc_channel_dispatcher.hpp",
        "edc_chat_bubble_page.hpp",
        "edc_chat_model.hpp",
        "edc_session_model.hpp",
        "edc_chat_transcript.hpp",
        "edc_chat_widget.hpp",
        "edc_chat_window.hpp",
        "edc_clipboard.hpp",
        "edc_command_palette.hpp",
        "edc_colorscheme_palette.hpp",
        "edc_setup_dialog.hpp",
        "edc_update_dialog.hpp",
        "edc_domain.hpp",
        "edc_history_line.hpp",
        "edc_input.hpp",
        "edc_message.hpp",
        "edc_modal.hpp",
        "edc_message_list.hpp",
        "edc_ports.hpp",
        "edc_theme.hpp",
        "edc_token_style.hpp",
        "edc_tui_window.hpp",
        "edc_scroll_view.hpp",
        "edc_sidebar.hpp",
        "edc_status_bar_view.hpp",
        "edc_style.hpp",
        "edc_task_snapshot.hpp",
        "edc_task_tree_model.hpp",
        "edc_task_widget.hpp",
        "edc_text_util.hpp",
        "edc_todo_widget.hpp",
        "edc_widget.hpp",
        "edc_working_indicator_view.hpp",
        "markdown.hpp",
    ],
    includes = ["."],
    deps = [
        "//src/app:api",
        "//src/app:app",
        "//src/common:common",
        "//src/common:syntax_headers",
        "//src/common:syntax_lang",
        # syntax highlighter (compiled into this archive above): tree-sitter core
        # plus the grammars; markdown.cpp tokenises fenced code into semantic spans.
        "@pcre2//:pcre2",  # edc_syntax.c's #match? predicate evaluation
        "@tree-sitter-bazel//:tree-sitter",
        "@tree-sitter-c//:tree-sitter-c",
        "@tree-sitter-cpp//:tree-sitter-cpp",
        "@tree-sitter-python//:tree-sitter-python",
        "@tree-sitter-bash//:tree-sitter-bash",
        "@tree-sitter-json//:tree-sitter-json",
        "@tree-sitter-go//:tree-sitter-go",
        "@tree-sitter-javascript//:tree-sitter-javascript",
        "@tree-sitter-rust//:tree-sitter-rust",
        "@tree-sitter-typescript//:tree-sitter-typescript",
        "@tree-sitter-typescript//:tree-sitter-tsx",
        "@tree-sitter-java//:tree-sitter-java",
        "@tree-sitter-css//:tree-sitter-css",
        "@tree-sitter-ruby//:tree-sitter-ruby",
        "@tree-sitter-toml//:tree-sitter-toml",
        "@tree-sitter-agda//:tree-sitter-agda",
        "@tree-sitter-c-sharp//:tree-sitter-c-sharp",
        "@tree-sitter-scala//:tree-sitter-scala",
        "@tree-sitter-diff//:tree-sitter-diff",
        "@tree-sitter-make//:tree-sitter-make",
        "@tree-sitter-r//:tree-sitter-r",
        "@tree-sitter-clojure//:tree-sitter-clojure",
        "@tree-sitter-hcl//:tree-sitter-hcl",
        "@tree-sitter-vim//:tree-sitter-vim",
        # edc_mention.h: the `@reference` grammar the input line paints is the one the
        # mention pass expands (see that header on why it is not duplicated here).
        "//src/shell:shell_parse",
        "@cjson//:cjson",
        "@ftxui//:dom",
        "@ftxui//:ftxui",
        "@libuv",
        "@log.c//:log.c",
        "@md4c//:md4c",
    ],
    visibility = ["//tests:__pkg__"],
)

# main.cpp is not in a library (it is the binary's only source), so the option parser has no
# target of its own for a test to depend on. tests/test_version reads it to cross-check the
# --help block against the flags the parser actually answers -- see that file for why.
filegroup(
    name = "main_cpp",
    srcs = ["main.cpp"],
    visibility = ["//tests:__pkg__"],
)

cc_binary(
    name = "edgeclaw",
    srcs = [
        "main.cpp",
    ],
    deps = [
        "//src/actor:actors",
        "//src/app:api",
        "//src/app:app",
        "//src/common:common",
        "//src/common:update_cli",
        "//src/common:utlist_fix",
        "//src/common:version",
        "//src/kernel:kernel",
        "//src/shell:shell",
        "//src/tui:tui",
        "@ftxui//:dom",
        "@libuv",
        "@log.c//:log.c",
    ] + select({
        "//:windows": [],
        "//conditions:default": ["//src/device:device"],
    }),
    linkstatic = True,
    linkopts = LINK_OPTS,
    local_defines = select({
        "//:debug": ["EDGECLAW_DEBUG"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
