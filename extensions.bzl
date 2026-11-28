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
    # Java / CSS / Ruby.
    grammar_repo(name = "tree-sitter-java", version = "0.23.5",
                 sha256 = "cb199e0faae4b2c08425f88cbb51c1a9319612e7b96315a174a624db9bf3d9f0")
    grammar_repo(name = "tree-sitter-css", version = "0.25.0",
                 sha256 = "03965344d8c0435dc54fb45b281578420bb7db8b99df4d34e7e74105a274cb79", has_scanner = True)
    grammar_repo(name = "tree-sitter-ruby", version = "0.23.1",
                 sha256 = "e7e49577ddc1f2de8e42d42353b477e338c15bbb95b2558e123ddc13d88789f0", has_scanner = True)
    grammar_repo(name = "tree-sitter-toml", version = "0.7.0",
                 sha256 = "7d52a7d4884f307aabc872867c69084d94456d8afcdc63b0a73031a8b29036dc",
                 has_scanner = True, org = "tree-sitter-grammars")
    # A-class languages (standard layout, ABI-compatible, self-contained scm).
    grammar_repo(name = "tree-sitter-agda", version = "1.3.3",
                 sha256 = "f09055ac4df99d8c27af4a80442abd0659b75babd46e8693a7dd9133d5711ef5", has_scanner = True)
    grammar_repo(name = "tree-sitter-c-sharp", version = "0.23.1",
                 sha256 = "c0b008dca3c6820604bf0853b9668ba034f9750d89d170ba834261e94e2cd917", has_scanner = True)
    grammar_repo(name = "tree-sitter-scala", version = "0.24.0",
                 sha256 = "6ba17e09ba035a4a1b19db7906b87ec2c56cf400073e8e7272e8bdc9413921f8", has_scanner = True)
    grammar_repo(name = "tree-sitter-diff", version = "0.1.0",
                 sha256 = "0d0414ac873c33154b458f4d62fe096aa966392ff416920a08c5629c765aaed3",
                 org = "tree-sitter-grammars")
    grammar_repo(name = "tree-sitter-make", version = "1.1.1",
                 sha256 = "31affca06041623cfa5f6a728cb3f0f2a8eb7fd01607e1a2681ea1f318367211",
                 org = "tree-sitter-grammars")
    grammar_repo(name = "tree-sitter-r", version = "1.2.0",
                 sha256 = "a95b6e79a40b6b906a3b61ec040c15422c549d599e01d87092bf7ae78ebcadc5",
                 has_scanner = True, org = "r-lib")
    grammar_repo(name = "tree-sitter-clojure", version = "0.0.1",
                 sha256 = "d933f33de79d6743631499fb0b643e2d40b3ba0a3abd0449a092d1cb84112ed1",
                 org = "anomalyco")

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
