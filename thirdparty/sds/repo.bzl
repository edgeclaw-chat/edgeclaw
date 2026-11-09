load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repo():
    git_repository(
        name = "sds",
        remote = "https://github.com/antirez/sds.git",
        commit = "5347739b1581fcba74fd5cab1fc21d2aef317d71",
        build_file = "//thirdparty/sds:sds.BUILD",
        # sds.h declares its five string headers with `struct __attribute__((packed))`,
        # which is GCC/Clang-only. Under MSVC -- the only Windows compiler this project
        # supports -- that is a syntax error, and because sds.h is included by nearly every
        # source file here, the failure cascaded into thousands of "use of undefined type
        # sdshdrN" errors in files that had nothing wrong with them. The patch keeps the
        # GCC spelling and adds a `#pragma pack(push, 1)` branch for MSVC; both produce the
        # same 3/5/9/17-byte header sizes, which is what SDS_HDR() relies on.
        # The patch is generated against the LF blob above, so it applies unchanged on a
        # machine whose checkout was rewritten to CRLF by core.autocrlf (both verified).
        patches = ["//patches:sds-msvc.patch"],
        patch_args = ["-p1"],
    )
