load('//thirdparty/log.c:repo.bzl', logc_repo = "repo")
load('//thirdparty/ccronexpr:repo.bzl', ccronexpr_repo = "repo")
load('//thirdparty/md4c:repo.bzl', md4c_repo = "repo")
load('//thirdparty/lwip:repo.bzl', lwip_repo = "repo")
load('//thirdparty/sds:repo.bzl', sds_repo = "repo")
load('//thirdparty/uthash:repo.bzl', uthash_repo = "repo")
load('//thirdparty/libuv:repo.bzl', libuv_repo = "repo")
load('//thirdparty/llhttp:repo.bzl', llhttp_repo = "repo")
load('//thirdparty/libyuarel:repo.bzl', libyuarel_repo = "repo")
load('//toolchain:arm_toolchain_repo.bzl', 'arm_toolchain_repo')
load('//toolchain:aarch64_toolchain_repo.bzl', 'aarch64_toolchain_repo')
load('//toolchain:android_toolchain_repo.bzl', 'aarch64_android_toolchain_repo', 'arm_android_toolchain_repo')
load('//toolchain:mingw_toolchain_repo.bzl', 'mingw_toolchain_repo')
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

def _mingw_toolchain_impl(module_ctx):
    mingw_toolchain_repo(name = "mingw_toolchain")

mingw_toolchain = module_extension(
    implementation = _mingw_toolchain_impl,
)

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
