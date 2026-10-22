/* What edc_sandbox_verify_path_at() returns for a path, when enforcement is OFF.
 *
 * With the sandbox off (or with no resolved root) the tools still go through this call:
 * it is what anchors a RELATIVE path at the session's bound working directory. That is
 * the whole job of the early-return branch -- and it has to leave an ABSOLUTE path
 * alone. It used to test `path[0] != '/'`, which is the same slash-only assumption that
 * made session binding break ("D:\ws" anchored at "D:\ws" became "D:\ws\D:\ws"), so on
 * Windows every absolute path handed to read_file / write_file / list_dir / edit_file /
 * @file-by-absolute-path was prefixed with the session directory and then failed to
 * resolve. edc_path_is_absolute() is the one predicate that knows about drive letters and
 * UNC roots; this pins that the branch asks it.
 *
 * Runs on every platform: the Windows-shaped inputs are checked on Windows only, but the
 * "absolute stays absolute" rule is asserted everywhere (a slash-only test also passes on
 * POSIX, which is why the original bug could only be seen on Windows). */

#include "edc_compat.h"
#include "edc_config.h"
#include "edc_sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            passed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* A config with enforcement OFF -- the state Windows always runs in (see
 * edc_platform_has_sandbox), and the state any platform runs in after /sandbox off. */
static struct edc_config unenforced_conf(void)
{
    struct edc_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.enable_sandbox = false;
    conf.sandbox_root_real = NULL;
    return conf;
}

/* Verify |path| against |base| and hand back the string the tools would open. */
static sds verify(struct edc_config *conf, const char *base, const char *path)
{
    EdcError *error = NULL;
    sds out = edc_sandbox_verify_path_at(conf, base, path, false, &error);
    if (error)
        edc_error_free(error);
    return out;
}

int main(void)
{
    struct edc_config conf = unenforced_conf();

#ifdef _WIN32
    const char *base = "D:\\github\\edgeclaw\\workspace";
    const char *abs = "D:\\github\\edgeclaw\\src\\main.c";
    const char *abs_other_drive = "C:\\Windows\\win.ini";
    const char *unc = "\\\\server\\share\\file.txt";
    const char *rel = "src\\main.c";
    const char *expect_rel = base;
#else
    const char *base = "/home/user/workspace";
    const char *abs = "/usr/include/stdio.h";
    const char *abs_other_drive = NULL;
    const char *unc = NULL;
    const char *rel = "src/main.c";
    const char *expect_rel = base;
#endif

    sds out = verify(&conf, base, abs);
    CHECK(out != NULL, "an absolute path comes back as a string");
    CHECK(out && strcmp(out, abs) == 0, "an absolute path is NOT anchored at the base");
    if (out)
        sdsfree(out);

    if (abs_other_drive)
    {
        out = verify(&conf, base, abs_other_drive);
        CHECK(out && strcmp(out, abs_other_drive) == 0, "an absolute path on another drive is NOT anchored");
        if (out)
            sdsfree(out);
    }

    if (unc)
    {
        out = verify(&conf, base, unc);
        CHECK(out && strcmp(out, unc) == 0, "a UNC path is NOT anchored");
        if (out)
            sdsfree(out);
    }

    /* The other half of the branch: a relative path IS anchored -- that is what makes
     * "@dir/name.md" and "read src/main.c" mean the session's directory. */
    out = verify(&conf, base, rel);
    CHECK(out != NULL && strncmp(out, expect_rel, strlen(expect_rel)) == 0 && out[strlen(expect_rel)] == PATH_SEP,
          "a relative path is anchored at the base");
    CHECK(out != NULL && strstr(out, rel) != NULL, "the relative part survives the anchor");
    if (out)
        sdsfree(out);

    /* No base: nothing to anchor at, the path is handed back as given. */
    out = verify(&conf, NULL, rel);
    CHECK(out != NULL && strcmp(out, rel) == 0, "without a base the path is returned as given");
    if (out)
        sdsfree(out);

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
