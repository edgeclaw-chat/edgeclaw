/* The path contract, in one place.
 *
 * Every Windows path bug this tree has had came from the same shape: one more copy of
 * "is A inside B" / "where does the directory part end" / "is this absolute", written with
 * a '/' literal somewhere the compiler could not complain. The fixes landed one copy at a
 * time, so the next copy was still wrong -- see edc_sandbox.c's path_prefix_match_len (a
 * slash-only strncmp, fixed everywhere EXCEPT there) and edc_app.c's raw
 * strncmp(parent, root, strlen(root)) (which also called "<ws>-evil" a child of "<ws>").
 *
 * Since then there is one primitive, edc_path_prefix_len(), and edc_path_is_within() is
 * its boolean form. This test pins the RULES, not the callers: the same table runs on
 * every platform, with the Windows-shaped rows compiled in only on Windows (a backslash is
 * an ordinary character on POSIX, and a table that pretended otherwise would have to lie
 * about one platform or the other).
 *
 * Free of /tmp and mkdtemp so it can run on Windows: the scratch directory comes from
 * TMPDIR -> TEMP -> TMP -> /tmp, named after the pid (compare test_path_absolute.c). */
#include "edc_compat.h"
#include "edc_error.h"
#include "edc_file_index.h"
#include "edc_file_utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static int passed = 0;
static int failed = 0;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    failed++;
}

/* ── edc_path_prefix_len / edc_path_is_within ─────────────────────────────── */

static void check_within(const char *path, const char *prefix, int want)
{
    const int got = edc_path_prefix_len(path, prefix);
    if (got != want)
        fail("prefix_len(\"%s\", \"%s\") = %d, want %d", path ? path : "(null)", prefix ? prefix : "(null)", got, want);
    else
        passed++;

    /* The boolean must agree with the primitive: that is the whole point of having one. */
    const bool within = edc_path_is_within(path, prefix);
    if (within != (want >= 0))
        fail("is_within(\"%s\", \"%s\") = %d, but prefix_len says %d", path ? path : "(null)",
             prefix ? prefix : "(null)", (int)within, want);
    else
        passed++;
}

static void test_prefix_len(void)
{
    /* Whole segments, and the prefix's own length is what comes back. */
    check_within("/a/b", "/a", 2);
    check_within("/a/b", "/a/", 2);  /* a trailing separator is ignored */
    check_within("/a", "/a", 2);
    check_within("/a", "/a/", 2);
    check_within("/a/b/c", "/a", 2);
    check_within("/", "/", 1);

    /* The boundary cases: this is what a strncmp() gets wrong. */
    check_within("/a/bc", "/a/b", -1); /* "bc" is not a child of "b" */
    check_within("/ab", "/a", -1);
    check_within("/a", "/a/b", -1); /* the path is shorter than the prefix */
    check_within("", "/a", -1);

    /* An empty prefix contains everything (0), NULL contains nothing. */
    check_within("/a", "", 0);
    check_within("/a", NULL, -1);
    check_within(NULL, "/a", -1);

#ifdef _WIN32
    /* The Windows rules: two separators, case-insensitive, drive letters, UNC. */
    check_within("D:\\ws\\x", "D:\\ws", 5);
    check_within("D:\\ws", "D:\\ws", 5);
    check_within("D:\\ws", "D:\\ws\\", 5);
    check_within("D:\\ws\\x", "d:\\WS", 5); /* realpath keeps the case it was given */
    check_within("D:/ws/x", "D:\\ws", 5);   /* either separator, on either side */
    check_within("D:\\ws\\x", "D:/ws", 5);
    check_within("\\\\srv\\share\\a", "\\\\srv\\share", 11);

    /* Still not a prefix-confusion: "D:\wsevil" is a DIFFERENT directory from "D:\ws". */
    check_within("D:\\wsevil\\x", "D:\\ws", -1);
    check_within("C:\\a", "D:\\a", -1);
#else
    /* On POSIX a backslash is an ordinary file-name character, so none of these match. */
    check_within("D:\\ws\\x", "D:\\ws", -1);
    check_within("/a/b", "/a\\b", -1);
#endif
}

/* ── edc_path_is_sep / edc_path_is_absolute ───────────────────────────────── */

static void test_predicates(void)
{
#ifdef _WIN32
    const char *abs_yes[] = {"D:\\x", "D:/x", "\\", "\\\\srv\\share", "/x", "/"};
    const char *abs_no[] = {"", ".", "x", "C:", "C:x", "..", "./x"};
#else
    const char *abs_yes[] = {"/x", "/", "/a/b"};
    const char *abs_no[] = {"", ".", "x", "C:", "C:\\x", "\\\\srv\\share", "..", "./x"};
#endif
    for (size_t i = 0; i < sizeof(abs_yes) / sizeof(abs_yes[0]); i++)
    {
        if (!edc_path_is_absolute(abs_yes[i]))
            fail("is_absolute(\"%s\") should be true", abs_yes[i]);
        else
            passed++;
    }
    for (size_t i = 0; i < sizeof(abs_no) / sizeof(abs_no[0]); i++)
    {
        if (edc_path_is_absolute(abs_no[i]))
            fail("is_absolute(\"%s\") should be false", abs_no[i]);
        else
            passed++;
    }
    if (edc_path_is_absolute(NULL))
        fail("is_absolute(NULL) should be false");
    else
        passed++;

    /* '\\' separates only on Windows; '/' does everywhere. */
    if (!edc_path_is_sep('/'))
        fail("'/' is a separator everywhere");
    else
        passed++;
#ifdef _WIN32
    if (!edc_path_is_sep('\\'))
        fail("'\\' is a separator on Windows");
    else
        passed++;
#else
    if (edc_path_is_sep('\\'))
        fail("'\\' is NOT a separator on POSIX");
    else
        passed++;
#endif
    if (edc_path_is_sep('a') || edc_path_is_sep('\0'))
        fail("ordinary characters are not separators");
    else
        passed++;
}

/* ── edc_path_join / edc_path_dirname ─────────────────────────────────────── */

static void expect_str(const char *got, const char *want, const char *what)
{
    if (!got || !want || strcmp(got, want) != 0)
        fail("%s = \"%s\", want \"%s\"", what, got ? got : "(null)", want ? want : "(null)");
    else
        passed++;
}

static void test_join_and_dirname(void)
{
    sds j;

    /* The join emits THIS platform's separator (it normalises an embedded one too). */
    char want[64];
    snprintf(want, sizeof(want), "a%cb", PATH_SEP);
    j = edc_path_join("a", "b", NULL);
    expect_str(j, want, "join(a, b)");
    sdsfree(j);

    snprintf(want, sizeof(want), "a%cb%cc.txt", PATH_SEP, PATH_SEP);
    j = edc_path_join("a", "b", "c.txt", NULL);
    expect_str(j, want, "join(a, b, c.txt)");
    sdsfree(j);

    j = edc_path_join("", "a", NULL); /* an empty segment is skipped, not doubled */
    expect_str(j, "a", "join(\"\", a)");
    sdsfree(j);

    j = edc_path_dirname("a/b/c.txt");
    expect_str(j, "a/b", "dirname(a/b/c.txt)");
    sdsfree(j);

    j = edc_path_dirname("a/b/");
    expect_str(j, "a/b", "dirname(a/b/)");
    sdsfree(j);

    j = edc_path_dirname("name");
    expect_str(j, ".", "dirname(name)");
    sdsfree(j);

    j = edc_path_dirname("/a");
    expect_str(j, "/", "dirname(/a)"); /* the root is not "." */
    sdsfree(j);

#ifdef _WIN32
    /* The join normalises to the platform separator, embedded ones included. */
    j = edc_path_join("D:\\ws", "sub", "f.txt", NULL);
    expect_str(j, "D:\\ws\\sub\\f.txt", "join(D:\\ws, sub, f.txt)");
    sdsfree(j);

    j = edc_path_join("D:\\ws", "a/b", NULL);
    expect_str(j, "D:\\ws\\a\\b", "join() rewrites an embedded '/'");
    sdsfree(j);

    j = edc_path_dirname("D:\\ws\\sub\\f.txt");
    expect_str(j, "D:\\ws\\sub", "dirname(D:\\ws\\sub\\f.txt)");
    sdsfree(j);

    j = edc_path_dirname("D:\\ws\\sub");
    expect_str(j, "D:\\ws", "dirname(D:\\ws\\sub)");
    sdsfree(j);
#endif
}

/* ── edc_dir_create_with_parents ──────────────────────────────────────────── */

/* A scratch directory under the platform's temp root (no mkdtemp: POSIX only). */
static bool scratch(char *buf, size_t cap, const char *leaf)
{
    const char *base = getenv("TMPDIR");
    if (!base || !base[0])
        base = getenv("TEMP");
    if (!base || !base[0])
        base = getenv("TMP");
    if (!base || !base[0])
        base = "/tmp";

    const int n = snprintf(buf, cap, "%s%cedc_contract_%ld%c%s", base, PATH_SEP, (long)getpid(), PATH_SEP, leaf);
    return n > 0 && (size_t)n < cap;
}

static void test_mkdir_p(void)
{
    char root[512], deep[600], sibling[600];

    if (!scratch(root, sizeof(root), "tree"))
    {
        fail("cannot build a scratch path");
        return;
    }

    EdcError *error = NULL;
    if (!edc_dir_create_with_parents(root, 0755, &error))
    {
        fail("mkdir_p(%s): %s", root, error ? error->message : "?");
        if (error)
            edc_error_free(error);
        return;
    }
    passed++;

    /* Three missing levels below an existing one: the split has to walk them, and on
     * Windows that means understanding the drive prefix and both separators. */
    snprintf(deep, sizeof(deep), "%s%ca%cb%cc", root, PATH_SEP, PATH_SEP, PATH_SEP);
    error = NULL;
    if (!edc_dir_create_with_parents(deep, 0755, &error))
    {
        fail("mkdir_p(%s): %s", deep, error ? error->message : "?");
        if (error)
            edc_error_free(error);
        return;
    }
    passed++;

    char probe[640];
    snprintf(probe, sizeof(probe), "%s%ca", root, PATH_SEP);
    expect_str(edc_file_is_dir(probe) ? "dir" : "missing", "dir", "mkdir_p created the first level");
    snprintf(probe, sizeof(probe), "%s%ca%cb%cc", root, PATH_SEP, PATH_SEP, PATH_SEP);
    expect_str(edc_file_is_dir(probe) ? "dir" : "missing", "dir", "mkdir_p created the last level");

    /* Idempotent: EEXIST is success, not an error (the second call must not fail). */
    error = NULL;
    if (!edc_dir_create_with_parents(deep, 0755, &error))
    {
        fail("mkdir_p is not idempotent: %s", error ? error->message : "?");
        if (error)
            edc_error_free(error);
    }
    else
        passed++;

    /* A mixed-separator spelling of the same tree (Windows only: elsewhere '\\' is just a
     * character in a file name, so there is nothing to normalise). */
#ifdef _WIN32
    snprintf(sibling, sizeof(sibling), "%s/a\\b2/c2", root);
    error = NULL;
    if (!edc_dir_create_with_parents(sibling, 0755, &error))
    {
        fail("mkdir_p(mixed separators): %s", error ? error->message : "?");
        if (error)
            edc_error_free(error);
    }
    else
        passed++;
#endif
}

/* ── the "@" completion's path handling ───────────────────────────────────── */

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        fail("cannot create %s", path);
        return;
    }
    fputs(text, f);
    fclose(f);
}

/* @ completion is where a typed path is split into "the directory to read" and "the
 * name being typed", and where a hit is shortened back to a root-relative path. Both
 * halves used a '/' literal: strrchr(q, '/') found no directory part at all for
 * "@sub\inner", and the relative-emit compared "[base_len] == '/'" so nothing was ever
 * shortened on Windows. tests/test_file_index.c cannot cover this -- it drives the tree
 * with system("rm -rf"), so it does not run here. */
static void test_index_query(void)
{
    char root[512], alpha[600], sub[600], inner[600];

    if (!scratch(root, sizeof(root), "idx"))
    {
        fail("cannot build a scratch path");
        return;
    }
    EdcError *error = NULL;
    if (!edc_dir_create_with_parents(root, 0755, &error))
    {
        fail("mkdir_p(%s): %s", root, error ? error->message : "?");
        if (error)
            edc_error_free(error);
        return;
    }

    snprintf(alpha, sizeof(alpha), "%s%calpha.txt", root, PATH_SEP);
    snprintf(sub, sizeof(sub), "%s%csub", root, PATH_SEP);
    snprintf(inner, sizeof(inner), "%s%csub%cinner.txt", root, PATH_SEP, PATH_SEP);
    write_text(alpha, "a\n");
    if (!edc_dir_create_with_parents(sub, 0755, NULL))
        fail("cannot create %s", sub);
    write_text(inner, "i\n");

    char buf[8192];

    /* An empty query lists the completion root, shortened to root-relative names: that is
     * what the picker inserts, so an absolute row here would be a bug of its own. */
    size_t rows = edc_file_index_query(root, "", buf, sizeof(buf));
    if (rows == 0)
        fail("query(\"\") returned no rows");
    else
        passed++;
    if (!strstr(buf, "alpha.txt"))
        fail("query(\"\") did not list alpha.txt: %s", buf);
    else
        passed++;
    if (strstr(buf, root))
        fail("query(\"\") emitted an absolute path instead of a root-relative one");
    else
        passed++;

    /* A typed prefix only narrows the listing. */
    rows = edc_file_index_query(root, "alp", buf, sizeof(buf));
    if (rows == 0 || !strstr(buf, "alpha.txt") || strstr(buf, "beta"))
        fail("query(\"alp\") should list alpha.txt only: %s", buf);
    else
        passed++;

    /* Descending into a directory: the query's separator is the platform's. */
#ifdef _WIN32
    rows = edc_file_index_query(root, "sub\\", buf, sizeof(buf));
#else
    rows = edc_file_index_query(root, "sub/", buf, sizeof(buf));
#endif
    if (rows == 0 || !strstr(buf, "inner.txt"))
        fail("query(\"sub%s\") should list inner.txt: %s", PATH_SEP == '\\' ? "\\" : "/", buf);
    else
        passed++;

    /* An ABSOLUTE query keeps absolute rows (there is no short relative form). A
     * trailing separator is what asks to descend into the directory rather than to
     * select the directory itself. */
    char q[600];
    snprintf(q, sizeof(q), "%s%c", root, PATH_SEP);
    rows = edc_file_index_query(root, q, buf, sizeof(buf));
    if (rows == 0 || !strstr(buf, alpha))
        fail("an absolute query should emit absolute rows containing %s: %s", alpha, buf);
    else
        passed++;
}

int main(void)
{
    uv_loop_t loop;
    if (uv_loop_init(&loop) != 0)
    {
        fprintf(stderr, "uv_loop_init failed\n");
        return 1;
    }
    edc_file_utils_init(&loop);

    test_prefix_len();
    test_predicates();
    test_join_and_dirname();
    test_mkdir_p();
    test_index_query();

    uv_loop_close(&loop);

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
