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

static void test_path_is_within(void)
{
    log_info("  TEST: edc_path_is_within");

    CHECK(edc_path_is_within("/a/b", "/a"), "child is within");
    CHECK(edc_path_is_within("/a", "/a"), "self is within");
    CHECK(!edc_path_is_within("/a/bc", "/a/b"), "segment boundary respected");
    CHECK(edc_path_is_within("/a/b", "/a/"), "trailing separator tolerated");
    CHECK(!edc_path_is_within("/a/b", "/a/bc"), "longer prefix is not a parent");
#ifdef _WIN32
    CHECK(edc_path_is_within("D:\\ws\\sub", "D:/ws"), "mixed separators");
    CHECK(edc_path_is_within("d:\\ws\\sub", "D:\\WS"), "case-insensitive");
    CHECK(!edc_path_is_within("D:\\wsx\\sub", "D:\\ws"), "segment boundary (windows)");
#endif
}

/* Nested creation and the containment guard, which used to fail on Windows: the guard
 * compared the resolved parent against the root with pr[len] == '/' only, so a real
 * backslash path was always "outside". */
static void test_nested_dir_and_write_containment(void)
{
    log_info("  TEST: nested directory creation + write containment");

    sds cwd = edc_cwd();
    char *root = cwd ? realpath(cwd, NULL) : NULL;
    CHECK(root != NULL, "cwd resolves");
    if (!root)
    {
        sdsfree(cwd);
        return;
    }

    sds nested = edc_path_join(root, ".edc_test_nested", "a", "b", "c", NULL);
    EdcError *err = NULL;
    bool ok = edc_dir_create_with_parents(nested, 0700, &err);
    CHECK(ok, "nested a/b/c created from scratch");
    if (!ok && err)
        log_error("    mkdir error: %s", err->message);
    if (err)
        edc_error_free(err);
    CHECK(edc_file_is_dir(nested), "every level exists");

    sds file = edc_path_join(nested, "note.txt", NULL);
    err = NULL;
    ok = edc_file_write_safe(file, "hi", 2, root, &err);
    CHECK(ok, "write inside the root is accepted");
    if (!ok && err)
        log_error("    write error: %s", err->message);
    if (err)
        edc_error_free(err);

    sds up = edc_path_dirname(root);
    sds outside = edc_path_join(up, "edc_test_should_not_exist.txt", NULL);
    err = NULL;
    ok = edc_file_write_safe(outside, "hi", 2, root, &err);
    CHECK(!ok, "write outside the root is refused");
    if (err)
        edc_error_free(err);

    remove(file);
    sds dir = sdsdup(nested);
    for (int i = 0; i < 4 && dir; i++) /* c, b, a, .edc_test_nested */
    {
        rmdir(dir);
        sds parent = edc_path_dirname(dir);
        sdsfree(dir);
        dir = parent;
    }
    sdsfree(dir);
    sdsfree(outside);
    sdsfree(up);
    sdsfree(file);
    sdsfree(nested);
    sdsfree(cwd);
    free(root);
}

int main(void)
{
    log_info("== test_path_absolute ==");

    test_absolute_predicate();
    test_absolute_workdir_is_not_anchored();
    test_path_is_within();
    test_nested_dir_and_write_containment();

    log_info("passed=%d failed=%d", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
