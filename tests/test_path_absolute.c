/* Regression test for `session new <dir>` on Windows.
 *
 * edc_session_resolve_workdir() used to treat only a leading '/' as absolute and anchored
 * everything else at the base, so an absolute Windows path ("D:\ws") became
 * "<base>\D:\ws", realpath failed, and creating a session with an explicit working
 * directory failed outright ("not a directory: D:\ws\D:\ws").
 *
 * The base used below does not exist on purpose: a relative path must fail through it, an
 * absolute one must resolve regardless. Kept free of /tmp and mkdtemp so it runs on every
 * platform -- tests/test_session_workdir.c hardcodes /tmp and cannot run on Windows. */
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_utils.h"
#include "edc_session.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_passed = 0;
static int test_failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            log_error("    FAIL: %s", msg);                                                                            \
            test_failed++;                                                                                             \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            test_passed++;                                                                                             \
        }                                                                                                              \
    } while (0)

static void test_absolute_predicate(void)
{
    log_info("  TEST: edc_path_is_absolute");

    CHECK(edc_path_is_absolute("/usr/bin"), "POSIX absolute");
    CHECK(edc_path_is_absolute("/"), "bare root");
    CHECK(!edc_path_is_absolute("relative/dir"), "relative");
    CHECK(!edc_path_is_absolute(""), "empty");
    CHECK(!edc_path_is_absolute(NULL), "NULL");

#ifdef _WIN32
    CHECK(edc_path_is_absolute("D:\\github\\edgeclaw"), "drive + backslash");
    CHECK(edc_path_is_absolute("C:/Users"), "drive + slash");
    CHECK(edc_path_is_absolute("\\\\server\\share"), "UNC");
    CHECK(!edc_path_is_absolute("D:"), "bare drive is drive-relative, not absolute");
    CHECK(!edc_path_is_absolute("D:dir"), "drive-relative form");
#endif
}

static void test_absolute_workdir_is_not_anchored(void)
{
    log_info("  TEST: absolute workdir is not anchored at a missing base");

    sds cwd = edc_cwd();
    char *real = cwd ? realpath(cwd, NULL) : NULL;
    CHECK(cwd != NULL && real != NULL, "cwd resolves");
    if (!cwd || !real)
    {
        sdsfree(cwd);
        free(real);
        return;
    }

    /* Absolute: must resolve even though the base is nonsense. */
    EdcError *err = NULL;
    sds resolved = edc_session_resolve_workdir("/no/such/base/edgeclaw-xyz", cwd, &err);
    CHECK(resolved != NULL, "absolute workdir resolves despite a bogus base");
    CHECK(resolved && strcmp(resolved, real) == 0, "resolved == realpath(cwd)");
    if (!resolved && err)
        log_error("    resolve error: %s", err->message);
    if (err)
        edc_error_free(err);
    sdsfree(resolved);

    /* Relative: must fail, which proves the bogus base was really used. */
    err = NULL;
    sds bad = edc_session_resolve_workdir("/no/such/base/edgeclaw-xyz", "relative-dir", &err);
    CHECK(bad == NULL, "relative workdir fails against the missing base");
    if (err)
        edc_error_free(err);
    sdsfree(bad);

    sdsfree(cwd);
    free(real);
}

int main(void)
{
    log_info("== test_path_absolute ==");

    test_absolute_predicate();
    test_absolute_workdir_is_not_anchored();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
