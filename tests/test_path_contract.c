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
#include "edc_path.h"
#include "edc_error.h"
#include "edc_file_index.h"
#include "edc_file_utils.h"

#include <inttypes.h> /* PRId64 / PRIu64 */
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
    const char *base = getenv("TEST_TMPDIR");
    if (!base || !*base)
        base = getenv("TMPDIR");
    if (!base || !base[0])
        base = getenv("TEMP");
    if (!base || !base[0])
        base = getenv("TMP");
    if (!base || !base[0])
        base = "/tmp";

    /* macOS sets TMPDIR to ".../T/" WITH a trailing separator; glibc's /tmp has none.
     * The format below adds one, so an unstripped base produces ".../T//edc_contract_..."
     * -- a double separator this test then asserts against, while the code under test
     * normalizes it away. Strip first, add exactly one after. */
    size_t base_len = strlen(base);
    while (base_len > 1 && base[base_len - 1] == PATH_SEP)
        base_len--;

    const int n = snprintf(buf, cap, "%.*s%cedc_contract_%" PRId64 "%c%s", (int)base_len, base, PATH_SEP,
                           (int64_t)getpid(),
                           PATH_SEP, leaf);
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
    /* rows are emitted in DISPLAY form, so on Windows the absolute row carries '/' */
    sds alpha_disp = edc_path_display(alpha);
    if (rows == 0 || !strstr(buf, alpha_disp ? alpha_disp : alpha))
        fail("an absolute query should emit absolute rows containing %s: %s", alpha_disp ? alpha_disp : alpha,
             buf);
    else
        passed++;
    if (alpha_disp)
        sdsfree(alpha_disp);
}

/* ── the normal form (edc_path_norm / _abs / _display / _equal) ─────────── */

static void expect_norm(const char *in, const char *want)
{
    /* |want| is written in display form ('/' for every separator) so ONE table covers both
     * platforms: the normal form uses the platform's separator, which is what this turns
     * the expectation into. |in| stays as written -- that is the input. */
    sds native = sdsnew(want);
    for (size_t i = 0; i < sdslen(native); i++)
        if (native[i] == '/')
            native[i] = PATH_SEP;

    sds got = edc_path_norm(in);
    expect_str(got ? got : "(null)", native, in && in[0] ? in : "(empty)");
    if (got)
        sdsfree(got);
    sdsfree(native);
}

/* The normal form is the promise the rest of the program relies on: one spelling per
 * directory, so a comparison, a dedup or a cache key can be a plain string compare.
 * It is lexical on purpose (edc_path.h says so): a path that does not exist yet still
 * has a normal form, which is what a write needs. */
static void test_normal_form(void)
{
    expect_norm("/a//b/", "/a/b");
    expect_norm("/a/./b", "/a/b");
    expect_norm("/a/b/../c", "/a/c");
    expect_norm("/", "/");
    expect_norm(".", ".");
    expect_norm("", ".");
    expect_norm("a/b", "a/b");

    /* An absolute path stops at its root: "/.." is "/", never "..". */
    expect_norm("/..", "/");
    expect_norm("/a/../../b", "/b");

    /* A relative one keeps what it cannot cancel. */
    expect_norm("../a", "../a");
    expect_norm("a/../../b", "../b");

#ifdef _WIN32
    /* One spelling: platform separators, no trailing one, except for a root. */
    expect_norm("D:/ws/x", "D:\\ws\\x");
    expect_norm("D:\\ws\\", "D:\\ws");
    expect_norm("D:\\", "D:\\");         /* the drive root keeps its separator */
    expect_norm("D:\\a\\..\\b", "D:\\b");
    expect_norm("\\\\srv\\share\\a\\..", "\\\\srv\\share");
    expect_norm("C:a\\..\\b", "C:b");    /* drive-RELATIVE stays drive-relative */
#else
    expect_norm("//a//b", "/a/b");
    expect_norm("/a\\b", "/a\\b");        /* '\\' is an ordinary character here */
#endif

    /* edc_path_abs: an absolute path is never joined onto the base (that is the bug it
     * replaced), a relative one always is. */
#ifdef _WIN32
    sds a = edc_path_abs("D:\\ws", "D:\\ws\\x");
    expect_str(a, "D:\\ws\\x", "abs(D:\\ws, D:\\ws\\x)");
    sdsfree(a);
    a = edc_path_abs("D:\\ws", "sub\\f.txt");
    expect_str(a, "D:\\ws\\sub\\f.txt", "abs(D:\\ws, sub\\f.txt)");
    sdsfree(a);
    a = edc_path_abs(NULL, "C:\\x");
    expect_str(a, "C:\\x", "abs(NULL, C:\\x)");
    sdsfree(a);
    /* display form: what goes into the input box / a mention */
    a = edc_path_display("D:\\ws\\src\\main.c");
    expect_str(a, "D:/ws/src/main.c", "display(D:\\ws\\src\\main.c)");
    sdsfree(a);
    /* equal: up to normal form, and case-insensitively */
    if (!edc_path_equal("D:\\ws", "d:/ws") || !edc_path_equal("D:\\ws\\", "D:\\ws") ||
        edc_path_equal("D:\\ws", "D:\\ws2") || edc_path_equal(NULL, "D:\\ws"))
        fail("edc_path_equal is wrong on Windows-shaped paths");
    else
        passed++;
#else
    sds a = edc_path_abs("/ws", "/ws/x");
    expect_str(a, "/ws/x", "abs(/ws, /ws/x)");
    sdsfree(a);
    a = edc_path_abs("/ws", "sub/f.txt");
    expect_str(a, "/ws/sub/f.txt", "abs(/ws, sub/f.txt)");
    sdsfree(a);
    a = edc_path_abs(NULL, "/x");
    expect_str(a, "/x", "abs(NULL, /x)");
    sdsfree(a);
    a = edc_path_display("/ws/src/main.c");
    expect_str(a, "/ws/src/main.c", "display keeps POSIX paths");
    sdsfree(a);
    if (!edc_path_equal("/ws/", "/ws") || edc_path_equal("/ws", "/ws2") || edc_path_equal(NULL, "/ws"))
        fail("edc_path_equal is wrong on POSIX-shaped paths");
    else
        passed++;
#endif

    if (edc_path_norm(NULL) != NULL)
        fail("norm(NULL) should be NULL");
    else
        passed++;
    if (edc_path_display(NULL) != NULL)
        fail("display(NULL) should be NULL");
    else
        passed++;
}
/* ── edc_dir_foreach skips "." and ".." ───────────────────────────────────── */

struct names_ctx
{
    int n;
    char seen[8][64];
};

static void collect_cb(const char *name, int type, void *ud)
{
    struct names_ctx *c = ud;
    (void)type;
    if (c->n < 8)
        snprintf(c->seen[c->n++], sizeof(c->seen[0]), "%s", name);
}

static bool seen_has(const struct names_ctx *c, const char *name)
{
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->seen[i], name) == 0)
            return true;
    return false;
}

/* The listing leaves out "." and ".." itself now. Three callers filtered them out with
 * their own copy of the test (the @ completion, the skills scan, the mention listing) and
 * a fourth -- the list_dir tool -- showed them to the model as if they were entries, so
 * the rule belongs to the traversal, not to four call sites. */
static void test_dir_foreach(void)
{
    char dir[512], f1[600], f2[600];

    if (!scratch(dir, sizeof(dir), "ls"))
    {
        fail("cannot build a scratch path");
        return;
    }
    EdcError *error = NULL;
    if (!edc_dir_create_with_parents(dir, 0755, &error))
    {
        fail("mkdir_p(%s): %s", dir, error ? error->message : "?");
        if (error)
            edc_error_free(error);
        return;
    }

    snprintf(f1, sizeof(f1), "%s%ca.txt", dir, PATH_SEP);
    snprintf(f2, sizeof(f2), "%s%cb.txt", dir, PATH_SEP);
    write_text(f1, "a\n");
    write_text(f2, "b\n");

    struct names_ctx c;
    memset(&c, 0, sizeof(c));
    error = NULL;
    const bool ok = edc_dir_foreach(dir, collect_cb, &c, &error);
    if (!ok)
        fail("edc_dir_foreach(%s): %s", dir, error ? error->message : "?");
    else
        passed++;
    if (error)
        edc_error_free(error);

    if (seen_has(&c, ".") || seen_has(&c, ".."))
        fail("the listing contains \".\" or \"..\": %.64s", c.n > 0 ? c.seen[0] : "");
    else
        passed++;
    if (!seen_has(&c, "a.txt") || !seen_has(&c, "b.txt"))
        fail("the listing is missing the real entries");
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
    test_normal_form();
    test_dir_foreach();

    uv_loop_close(&loop);

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
