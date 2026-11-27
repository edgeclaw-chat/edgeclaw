load('//thirdparty/log.c:repo.bzl', logc_repo = "repo")
load('//thirdparty/ccronexpr:repo.bzl', ccronexpr_repo = "repo")
load('//thirdparty/md4c:repo.bzl', md4c_repo = "repo")
load('//thirdparty/lwip:repo.bzl', lwip_repo = "repo")
load('//thirdparty/sds:repo.bzl', sds_repo = "repo")
load('//thirdparty/uthash:repo.bzl', uthash_repo = "repo")
load('//thirdparty/libuv:repo.bzl', libuv_repo = "repo")
load('//thirdparty/llhttp:repo.bzl', llhttp_repo = "repo")
load('//thirdparty/libyuarel:repo.bzl', libyuarel_repo = "repo")
load('//thirdparty/tree-sitter-c:repo.bzl', tree_sitter_c_repo = "repo")
load('//thirdparty/tree-sitter-grammar:repo.bzl', grammar_repo = "grammar_repo")
load('//thirdparty/tree-sitter-typescript:repo.bzl', typescript_repo = "repo")
load('//toolchain:arm_toolchain_repo.bzl', 'arm_toolchain_repo')
load('//toolchain:aarch64_toolchain_repo.bzl', 'aarch64_toolchain_repo')
load('//toolchain:android_toolchain_repo.bzl', 'aarch64_android_toolchain_repo', 'arm_android_toolchain_repo')
load('//toolchain:musl_toolchain_repo.bzl', 'musl_toolchain_repo')

def _bc_thirdparty_deps_impl(module_ctx):
    logc_repo()
    ccronexpr_repo()
    md4c_repo()
    lwip_repo()
    sds_repo()
    uthash_repo()
    libuv_repo()
    llhttp_repo()
    libyuarel_repo()
    tree_sitter_c_repo()
    # Syntax-highlighting grammars (see //src/common:syntax). Each is pinned to
    # an official release by sha256; queries are embedded separately.
    grammar_repo(name = "tree-sitter-cpp", version = "0.23.4",
                 sha256 = "7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051", has_scanner = True)
    grammar_repo(name = "tree-sitter-python", version = "0.25.0",
                 sha256 = "4609a3665a620e117acf795ff01b9e965880f81745f287a16336f4ca86cf270c", has_scanner = True)
    grammar_repo(name = "tree-sitter-bash", version = "0.25.1",
                 sha256 = "2e785a761225b6c433410ef9c7b63cfb0a4e83a35a19e0f2aec140b42c06b52d", has_scanner = True)
    grammar_repo(name = "tree-sitter-json", version = "0.24.8",
                 sha256 = "acf6e8362457e819ed8b613f2ad9a0e1b621a77556c296f3abea58f7880a9213")
    grammar_repo(name = "tree-sitter-go", version = "0.25.0",
                 sha256 = "2dc241b97872c53195e01b86542b411a3c1a6201d9c946c78d5c60c063bba1ef")
    # P5: JavaScript, Rust, and TypeScript (TS + TSX from one archive).
    grammar_repo(name = "tree-sitter-javascript", version = "0.25.0",
                 sha256 = "9712fc283d3dc01d996d20b6392143445d05867a7aad76fdd723824468428b86", has_scanner = True)
    grammar_repo(name = "tree-sitter-rust", version = "0.24.2",
                 sha256 = "061e90a539a55a6aa65dceb0ad6425c50ab1a6e3e6d4ba430e2795ed4550f10e", has_scanner = True)
    typescript_repo()

bc_thirdparty_deps = module_extension(
    implementation = _bc_thirdparty_deps_impl,
)

def _arm_toolchain_impl(module_ctx):
    arm_toolchain_repo(name = "arm_toolchain")

arm_toolchain = module_extension(
    implementation = _arm_toolchain_impl,
)

def _aarch64_toolchain_impl(module_ctx):
    aarch64_toolchain_repo(name = "aarch64_toolchain")

aarch64_toolchain = module_extension(
    implementation = _aarch64_toolchain_impl,
)

def _aarch64_android_toolchain_impl(module_ctx):
    aarch64_android_toolchain_repo(name = "aarch64_android_toolchain")

aarch64_android_toolchain = module_extension(
    implementation = _aarch64_android_toolchain_impl,
)

def _arm_android_toolchain_impl(module_ctx):
    arm_android_toolchain_repo(name = "arm_android_toolchain")

arm_android_toolchain = module_extension(
    implementation = _arm_android_toolchain_impl,
)

# The Windows toolchain needs no repository rule and no module extension: Windows is built
# with MSVC, which Bazel autodetects by itself. The `mingw_toolchain` extension that used to
# be declared here (and registered from MODULE.bazel) was removed along with MinGW support.

# One repo per target arch, deliberately NOT registered via register_toolchains():
# they are only reachable through `--extra_toolchains` in the musl_* configs, so a
# machine without a musl toolchain is unaffected. See toolchain/musl_toolchain_repo.bzl.
def _musl_toolchain_impl(module_ctx):
    musl_toolchain_repo(name = "musl_toolchain_aarch64", arch = "aarch64")
    musl_toolchain_repo(name = "musl_toolchain_arm", arch = "arm")
    musl_toolchain_repo(name = "musl_toolchain_amd64", arch = "amd64")

musl_toolchain = module_extension(
    implementation = _musl_toolchain_impl,
)
