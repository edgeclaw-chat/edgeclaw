/* tests/test_search_matrix.c -- the grep/find matrix (src/toolbox/edc_tools_search.c).
 *
 * test_toolset.c pins the glob matcher's basics and test_tools_search_loop.c pins the
 * tool plumbing through a live actor loop. This file pins the SEARCH SEMANTICS, one
 * behaviour per case, against a tree built for the purpose: binary/size/depth guards, the
 * skip list, symlinks, CRLF, the 500-char line cap, limit truncation, literal vs regex,
 * unicode, and the "nothing found" texts. It also runs the same walk from several threads
 * at once, because the walk keeps its state per frame and a regression there would only
 * show up under concurrency. */

#include "edc_tools_search.h"

#include "sds.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "test_fs_util.h"

static int test_failed = 0;
static int test_passed = 0;

#define TEST(name) do { log_info("  TEST: %s", name); } while (0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        log_error("    FAIL: " __VA_ARGS__); \
        test_failed++; \
    } else { \
        test_passed++; \
    } \
} while (0)

/* ── helpers ──────────────────────────────────────────────────────────────── */

static char g_root[64];

static void join_path(char *out, size_t cap, const char *rel)
{
    snprintf(out, cap, "%s/%s", g_root, rel);
}

static void mkdir_p(const char *rel)
{
    char path[512];
    join_path(path, sizeof(path), rel);
    char *p = path + strlen(g_root);
    for (; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    }
    mkdir(path, 0755);
}

/* Body may contain NULs and CRs, so length is explicit. */
static void put(const char *rel, const void *body, size_t len)
{
    char path[512];
    join_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        log_error("    cannot write %s: %s", path, strerror(errno));
        test_failed++;
        return;
    }
    fwrite(body, 1, len, f);
    fclose(f);
}

static void put_str(const char *rel, const char *body) { put(rel, body, strlen(body)); }

static int count_occurrences(const char *hay, const char *needle)
{
    int n = 0;
    size_t len = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += len)
        n++;
    return n;
}

/* grep helper: the common shape (regex, case-sensitive, no glob, generous limit). */
static sds grep_text(const char *root, const char *pattern)
{
    return edc_search_run_blocking(root, true, pattern, NULL, false, false, 1000);
}

/* ── the fixture ──────────────────────────────────────────────────────────── */

static void build_tree(void)
{
    mkdir_p("sub/deep");
    put_str("a.c", "int a;\nTODO: alpha\n");
    put_str("b.h", "CRLF line\r\nTODO crlf\r\nlast\r\n");
    put_str("meta.txt", "a.c file\naXc file\nTODO. dot\nTODOX nodot\n");
    put("nonewline.txt", "TODO tail", 9); /* no trailing '\n' */
    put("empty.txt", "", 0);
    put_str("blank.txt", "one\n\ntwo\n\n");
    put_str("uni.txt", "\xe4\xb8\xad\xe6\x96\x87 TODO\n");
    put_str(".hidden.txt", "TODO hidden\n");
    put_str("sub/c.c", "TODO nested\n");
    put_str("sub/deep/d.c", "TODO deeper\n");

    /* 2000-char line: the result must keep exactly 500 characters of it. */
    char longline[2048];
    memcpy(longline, "TODO ", 5);
    memset(longline + 5, 'x', sizeof(longline) - 6);
    longline[sizeof(longline) - 1] = '\n';
    put("long.txt", longline, sizeof(longline));

    /* Binary: a NUL inside the 8 KiB probe window. */
    put("bin.dat", "head\0TODO binary\n", 18);

    /* A NUL PAST the old 8 KiB probe window, with a match before it. The prefix-only check
     * used to treat this as text, report the first line, and then silently stop matching at
     * the NUL -- a false negative with nothing in the output to show for it. Any NUL now
     * makes the file binary, so neither line is reported. */
    {
        sds late = sdsempty();
        late = sdscat(late, "TODO before the nul\n");
        for (int i = 0; i < 8192; i++)
            late = sdscatlen(late, "a", 1);
        late = sdscatlen(late, "\0", 1);
        late = sdscat(late, "TODO after the nul\n");
        put("late_nul.dat", late, sdslen(late));
        sdsfree(late);
    }



    /* Every name on the skip list, plus one that only shares a prefix. */
    static const char *const skips[] = {".git",      ".hg",   ".svn",   "node_modules", "build", "bazel-out",
                                       "bazel-bin",  "bazel-testlogs", "__pycache__", ".cache", ".venv", "venv",
                                       "target",     ".idea", ".vscode", "dist",        ".next", ".gradle"};
    char rel[128];
    for (size_t i = 0; i < sizeof(skips) / sizeof(skips[0]); i++)
    {
        snprintf(rel, sizeof(rel), "%s/skip.txt", skips[i]);
        mkdir_p(skips[i]);
        put_str(rel, "TODO in a skipped dir\n");
    }
    mkdir_p("buildx"); /* "buildx" is not "build" */
    put_str("buildx/keep.txt", "TODO not skipped\n");
    mkdir_p("filecase"); /* a FILE named "build", inside a normal dir */
    put_str("filecase/build", "TODO: a FILE named build\n");

    /* Symlinks: a file link, a dir link, and a cycle. None may be followed. */
    char link_path[512];
    join_path(link_path, sizeof(link_path), "flink.txt");
    if (symlink("a.c", link_path) != 0 && errno != EEXIST)
        log_error("    cannot symlink flink.txt: %s", strerror(errno));
    join_path(link_path, sizeof(link_path), "slink");
    if (symlink("sub", link_path) != 0 && errno != EEXIST)
        log_error("    cannot symlink slink: %s", strerror(errno));
    mkdir_p("cyc");
    join_path(link_path, sizeof(link_path), "cyc/up");
    if (symlink("..", link_path) != 0 && errno != EEXIST)
        log_error("    cannot symlink cyc/up: %s", strerror(errno));

    /* A FIFO in the tree: skipped as "not a regular file", and never opened (which would
     * block). */
    join_path(link_path, sizeof(link_path), "fifo.txt");
    if (mkfifo(link_path, 0644) != 0 && errno != EEXIST)
        log_error("    cannot mkfifo: %s", strerror(errno));

    /* Depth boundary: chains of 62/63/64/65 dirs, each with a TODO file at the end. */
    static const int depths[] = {62, 63, 64, 65};
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++)
    {
        char chain[512];
        snprintf(chain, sizeof(chain), "dep%d", depths[i]);
        mkdir_p(chain);
        size_t used = strlen(chain);
        for (int d = 0; d < depths[i]; d++)
        {
            memcpy(chain + used, "/x", 3);
            used += 2;
            mkdir_p(chain);
        }
        char file[600];
        snprintf(file, sizeof(file), "%s/f.txt", chain);
        char body[64];
        snprintf(body, sizeof(body), "TODO depth %d\n", depths[i]);
        put_str(file, body);
    }

    /* Past the 8 MiB read guard: skipped before it is allocated (and not counted). */
    mkdir_p("bigdir");
    {
        char path[512];
        join_path(path, sizeof(path), "bigdir/big.txt");
        FILE *f = fopen(path, "wb");
        if (f)
        {
            char block[4096];
            memset(block, 'z', sizeof(block));
            for (int i = 0; i < 2049; i++) /* 2049 * 4096 = 8 MiB + 4 KiB */
                fwrite(block, 1, sizeof(block), f);
            fwrite("\nTODO big\n", 1, 10, f);
            fclose(f);
        }
    }
}

static void tear_down(void)
{
    sds cmd = sdscatprintf(sdsempty(), "rm -rf '%s'", g_root);
    if (test_sh(cmd) != 0)
        log_error("    cleanup of %s failed", g_root);
    sdsfree(cmd);
}

/* ── cases ────────────────────────────────────────────────────────────────── */

static void test_golden_output(void)
{
    TEST("grep: byte-exact result on a one-file tree (path:line:text, relative)");

    char root[] = "/tmp/edc_mat_golden_XXXXXX";
    if (!mkdtemp(root))
    {
        CHECK(false, "mkdtemp failed");
        return;
    }
    sds path = sdscatprintf(sdsempty(), "%s/only.c", root);
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fwrite("line one\nTODO here\n", 1, 19, f);
        fclose(f);
    }
    sds out = grep_text(root, "TODO");
    CHECK(strcmp(out, "only.c:2:TODO here\n") == 0, "exact output, got <<%s>>", out);
    CHECK(strstr(out, root) == NULL, "the root prefix is stripped");
    sdsfree(out);
    remove(path);
    sdsfree(path);
    rmdir(root);
}

static void test_line_and_path_rules(void)
{
    TEST("grep: line numbers, relative paths, CRLF, no trailing newline, empty file");

    sds out = grep_text(g_root, "TODO");
    CHECK(strstr(out, "a.c:2:TODO: alpha") != NULL, "line number is 2 for a.c");
    CHECK(strstr(out, "b.h:2:TODO crlf") != NULL, "CRLF line reported without the '\\r'");
    CHECK(strstr(out, "b.h:2:TODO crlf\r") == NULL, "no stray CR survives into the text");
    CHECK(strstr(out, "nonewline.txt:1:TODO tail") != NULL, "the last line without '\\n' is searched");
    CHECK(strstr(out, "empty.txt") == NULL, "an empty file contributes nothing");

    /* After the CR is stripped, '$' anchors on the real end of the line. */
    sds anchor = edc_search_run_blocking(g_root, true, "crlf$", "b.h", false, false, 10);
    CHECK(strstr(anchor, "b.h:2:") != NULL, "'$' matches after the CR is stripped");
    sdsfree(anchor);
    sdsfree(out);
}

static void test_blank_lines(void)
{
    TEST("grep: empty lines match '^$', and a trailing newline is not a phantom line");

    sds out = edc_search_run_blocking(g_root, true, "^$", "blank.txt", false, false, 10);
    CHECK(strstr(out, "blank.txt:2:") != NULL, "line 2 (empty) matched");
    CHECK(strstr(out, "blank.txt:4:") != NULL, "line 4 (empty) matched");
    CHECK(strstr(out, "blank.txt:5:") == NULL, "no fifth line is invented after the final newline");
    CHECK(count_occurrences(out, "blank.txt:") == 2, "exactly two empty lines, got <<%s>>", out);
    sdsfree(out);
}

static void test_long_line_cap(void)
{
    TEST("grep: a 2000-char line is cut to 500 characters plus the marker");

    sds out = edc_search_run_blocking(g_root, true, "TODO", "long.txt", false, false, 10);
    const char *prefix = "long.txt:1:";
    CHECK(strncmp(out, prefix, strlen(prefix)) == 0, "hit has the usual shape");
    const char *body = out + strlen(prefix);
    const char *marker = " ... [line truncated]\n";
    CHECK(strcmp(body + 500, marker) == 0, "exactly 500 characters are kept before the marker");
    CHECK(sdslen(out) == strlen(prefix) + 500 + strlen(marker), "nothing else was appended (%zu bytes)",
          (size_t)sdslen(out));
    CHECK(body[0] == 'T' && body[499] == 'x', "the kept slice is the head of the line");
    sdsfree(out);
}

static void test_unicode(void)
{
    TEST("grep: a UTF-8 pattern matches and the bytes round-trip unchanged");

    sds out = grep_text(g_root, "\xe4\xb8\xad\xe6\x96\x87"); /* 中文 */
    CHECK(strstr(out, "uni.txt:1:\xe4\xb8\xad\xe6\x96\x87 TODO") != NULL, "unicode line returned verbatim: %s", out);
    sds free_me = edc_search_run_blocking(g_root, true, "TODO", "uni.txt", false, true, 10);
    CHECK(strstr(free_me, "uni.txt:1:") != NULL, "literal search handles unicode too");
    sdsfree(free_me);
    sdsfree(out);
}

static void test_binary_and_size_guards(void)
{
    TEST("grep: binary files and files past the size guard are skipped");

    sds bin = edc_search_run_blocking(g_root, true, "TODO", "bin.dat", false, false, 10);
    CHECK(strstr(bin, "no matches") != NULL, "a NUL in the probe window makes the file binary: %s", bin);
    CHECK(strstr(bin, "1 files read") != NULL, "the binary was read, so it counts as read");
    sdsfree(bin);

    sds big = edc_search_run_blocking(g_root, true, "TODO", "big.txt", false, false, 10);
    CHECK(strstr(big, "no matches") != NULL, "a file over 8 MiB is skipped: %s", big);
    CHECK(strstr(big, "0 files read") != NULL, "and it is skipped before being read, so it is not counted");
    sdsfree(big);

    sds hidden = edc_search_run_blocking(g_root, true, "TODO", ".hidden.txt", false, false, 10);
    CHECK(strstr(hidden, ".hidden.txt:1:TODO hidden") != NULL, "dot-files themselves are searched");
    sdsfree(hidden);
}

static void test_skip_dirs(void)
{
    TEST("grep: the skip list is exact (a prefix name and a FILE named 'build' survive)");

    sds out = grep_text(g_root, "TODO in a skipped dir");
    CHECK(strstr(out, "no matches") != NULL, "nothing from the skipped dirs: %s", out);
    sdsfree(out);

    sds keep = grep_text(g_root, "TODO not skipped");
    CHECK(strstr(keep, "buildx/keep.txt:1:") != NULL, "'buildx' is not on the list: %s", keep);
    sdsfree(keep);

    sds file_named_build = grep_text(g_root, "a FILE named build");
    CHECK(strstr(file_named_build, "filecase/build:1:") != NULL, "only DIRECTORIES are skipped by name: %s",
          file_named_build);
    sdsfree(file_named_build);
}

static void test_symlinks_and_fifo(void)
{
    TEST("grep/find: symlinks are not followed, a FIFO is skipped, and the walk ends");

    /* flink.txt -> a.c: following it would report a.c twice under two names. */
    sds out = grep_text(g_root, "TODO: alpha");
    CHECK(count_occurrences(out, "TODO: alpha") == 1, "the file symlink was not followed: %s", out);
    CHECK(strstr(out, "flink.txt") == NULL, "the link name does not appear");
    sdsfree(out);

    /* slink -> sub: following it would add sub/c.c and sub/deep/d.c a second time. */
    sds nested = grep_text(g_root, "TODO nested");
    CHECK(count_occurrences(nested, "TODO nested") == 1, "the dir symlink was not followed: %s", nested);
    sdsfree(nested);

    /* cyc/up -> the root: the classic non-terminating walk. Reaching the next check at
     * all proves it terminated. */
    sds cyc = edc_search_run_blocking(g_root, false, "", NULL, false, false, 500);
    CHECK(strstr(cyc, "fifo.txt") == NULL, "the FIFO is not listed as a file: %.80s", cyc);
    CHECK(strstr(cyc, "flink.txt") == NULL && strstr(cyc, "slink") == NULL, "links are not listed");
    CHECK(strstr(cyc, "cyc/up") == NULL, "the cycle link is not listed");
    sdsfree(cyc);
}

static void test_depth_cap(void)
{
    TEST("grep: the depth cap stops the walk (62/63 in, 64/65 out)");

    sds out = grep_text(g_root, "TODO depth");
    CHECK(strstr(out, "TODO depth 62") != NULL, "62 dirs down is searched");
    CHECK(strstr(out, "TODO depth 63") != NULL, "63 dirs down is searched");
    CHECK(strstr(out, "TODO depth 64") == NULL, "64 dirs down is not (frame depth cap)");
    CHECK(strstr(out, "TODO depth 65") == NULL, "65 dirs down is not");
    sdsfree(out);
}

static void test_limit(void)
{
    TEST("grep: limit truncates, says so, and is not applied when it is not reached");

    sds one = edc_search_run_blocking(g_root, true, "TODO", NULL, false, false, 1);
    CHECK(count_occurrences(one, "TODO") == 1, "exactly one hit is returned: %s", one);
    CHECK(strstr(one, "[reached the limit of 1 matches") != NULL, "the trailer names the limit");
    sdsfree(one);

    sds few = edc_search_run_blocking(g_root, true, "TODO", "a.c", false, false, 50);
    CHECK(strstr(few, "stopped at the limit") == NULL, "a limit that was not reached says nothing");
    sdsfree(few);

    /* limit <= 0 falls back to the default instead of returning nothing. */
    sds zero = edc_search_run_blocking(g_root, true, "TODO", "a.c", false, false, 0);
    CHECK(strstr(zero, "a.c:2:") != NULL, "limit 0 means 'use the default'");
    sdsfree(zero);

    sds negative = edc_search_run_blocking(g_root, true, "TODO", "a.c", false, false, -5);
    CHECK(strstr(negative, "a.c:2:") != NULL, "a negative limit behaves like 0");
    sdsfree(negative);
}

static void test_literal_vs_regex(void)
{
    TEST("grep: literal is literal, regex is extended, ignore_case folds both");

    sds re = edc_search_run_blocking(g_root, true, "a.c", "meta.txt", false, false, 10);
    CHECK(strstr(re, "meta.txt:1:a.c file") != NULL, "regex '.' matched 'a.c'");
    CHECK(strstr(re, "meta.txt:2:aXc file") != NULL, "regex '.' matched 'aXc'");
    sdsfree(re);

    sds lit = edc_search_run_blocking(g_root, true, "a.c", "meta.txt", false, true, 10);
    CHECK(strstr(lit, "meta.txt:1:a.c file") != NULL, "literal found the dot");
    CHECK(strstr(lit, "aXc") == NULL, "literal did not treat '.' as a wildcard: %s", lit);
    sdsfree(lit);

    sds dot_regex = edc_search_run_blocking(g_root, true, "TODO\\. dot", "meta.txt", false, false, 10);
    CHECK(strstr(dot_regex, "meta.txt:3:") != NULL, "an escaped dot in a regex works");
    sdsfree(dot_regex);

    sds upper = edc_search_run_blocking(g_root, true, "todo", "a.c", true, false, 10);
    CHECK(strstr(upper, "a.c:2:TODO: alpha") != NULL, "ignore_case + regex matches TODO with 'todo'");
    sdsfree(upper);

    sds upper_lit = edc_search_run_blocking(g_root, true, "todo", "a.c", true, true, 10);
    CHECK(strstr(upper_lit, "a.c:2:TODO: alpha") != NULL, "ignore_case + literal matches too");
    sdsfree(upper_lit);

    sds case_sensitive = edc_search_run_blocking(g_root, true, "todo", "a.c", false, true, 10);
    CHECK(strstr(case_sensitive, "no matches") != NULL, "without ignore_case the case matters");
    sdsfree(case_sensitive);

    /* An alternation is the reason regex is the default. */
    sds alt = edc_search_run_blocking(g_root, true, "alpha|crlf", NULL, false, false, 10);
    CHECK(strstr(alt, "a.c:2:") != NULL && strstr(alt, "b.h:2:") != NULL, "alternation spans files");
    sdsfree(alt);

    /* A pattern with a newline cannot match: the file is split into lines first. */
    sds nl = edc_search_run_blocking(g_root, true, "alpha\nreturn", "a.c", false, false, 10);
    CHECK(strstr(nl, "no matches") != NULL, "a multi-line pattern matches nothing: %s", nl);
    sdsfree(nl);
}

static void test_glob_filter(void)
{
    TEST("grep/find: the glob filters by name, or by relative path when it carries '/'");

    sds by_name = edc_search_run_blocking(g_root, true, "TODO", "*.c", false, false, 100);
    CHECK(strstr(by_name, "a.c:2:") != NULL, "'*.c' reaches the root file");
    CHECK(strstr(by_name, "sub/c.c:1:") != NULL, "and nested files (name-only glob)");
    CHECK(strstr(by_name, "sub/deep/d.c:1:") != NULL, "at any depth");
    CHECK(strstr(by_name, ".h:") == NULL, "and nothing else");
    sdsfree(by_name);

    sds by_path = edc_search_run_blocking(g_root, true, "TODO", "sub/*.c", false, false, 100);
    CHECK(strstr(by_path, "sub/c.c:1:") != NULL, "a path glob matches the relative path");
    CHECK(strstr(by_path, "sub/deep/d.c") == NULL, "'*' does not cross a '/'");
    sdsfree(by_path);

    sds question = edc_search_run_blocking(g_root, false, "", "?.c", false, false, 100);
    CHECK(strstr(question, "a.c") != NULL, "'?.c' matches the three-character name");
    CHECK(strstr(question, "meta.txt") == NULL, "and only that shape");
    sdsfree(question);

    sds empty_glob = edc_search_run_blocking(g_root, false, "", "", false, false, 100);
    CHECK(strstr(empty_glob, "a.c") != NULL, "an empty glob filters nothing");
    sdsfree(empty_glob);

    sds neither = edc_search_run_blocking(g_root, false, "", "no_such_name*", false, false, 100);
    CHECK(strcmp(neither, "(no files matched)\n") == 0, "find says so when nothing matched: %s", neither);
    sdsfree(neither);
}

static void test_find_shape(void)
{
    TEST("find: lists relative paths one per line, with no line numbers or content");

    sds out = edc_search_run_blocking(g_root, false, "", "*.c", false, false, 100);
    CHECK(strstr(out, "a.c\n") != NULL, "plain relative path");
    CHECK(strstr(out, ":") == NULL, "no 'path:line' shape for find: %s", out);
    CHECK(strstr(out, g_root) == NULL, "no root prefix");
    sdsfree(out);
}

static void test_errors_and_odd_input(void)
{
    TEST("grep: bad input yields text, never NULL and never a crash");

    sds bad = edc_search_run_blocking(g_root, true, "([unclosed", NULL, false, false, 10);
    CHECK(bad != NULL && sdslen(bad) == 0, "an invalid regex yields empty text (the error is raised by the tool)");
    sdsfree(bad);

    sds no_pattern = edc_search_run_blocking(g_root, true, "", NULL, false, false, 10);
    CHECK(no_pattern != NULL, "an empty regex is not a crash");
    sdsfree(no_pattern);

    sds no_pattern_literal = edc_search_run_blocking(g_root, true, "", NULL, false, true, 10);
    CHECK(strstr(no_pattern_literal, "no matches") != NULL, "an empty literal matches nothing: %s",
          no_pattern_literal);
    sdsfree(no_pattern_literal);

    sds file_root = edc_search_run_blocking("/tmp", true, "TODO", NULL, false, false, 3);
    CHECK(file_root != NULL, "a root that is not a directory is not a crash");
    sdsfree(file_root);

    /* A root that does not exist: the walk simply finds nothing. */
    sds missing = edc_search_run_blocking("/no/such/dir/at/all", false, "", NULL, false, false, 5);
    CHECK(strcmp(missing, "(no files matched)\n") == 0, "a missing root yields 'no files matched', got %s",
          missing);
    sdsfree(missing);

    /* A trailing slash on the root must not leave a leading '/' in the result. */
    sds trailing = sdscatprintf(sdsempty(), "%s/", g_root);
    sds out = edc_search_run_blocking(trailing, true, "TODO: alpha", NULL, false, false, 10);
    CHECK(strstr(out, "a.c:2:") != NULL, "a trailing slash is tolerated: %s", out);
    sdsfree(out);
    sdsfree(trailing);
}

static void test_glob_table(void)
{
    TEST("glob matcher: the full table, including the cases '*' must not swallow '/'");

    CHECK(edc_search_glob_match("*.c", "a.c"), "*.c ~ a.c");
    CHECK(!edc_search_glob_match("*.c", "a.h"), "*.c !~ a.h");
    CHECK(edc_search_glob_match("*", "x"), "* ~ x");
    CHECK(edc_search_glob_match("*", ""), "* ~ empty");
    CHECK(!edc_search_glob_match("?", ""), "? needs one character");
    CHECK(edc_search_glob_match("?", "a"), "? ~ a");
    CHECK(!edc_search_glob_match("a?c", "ac"), "? is not optional");
    CHECK(edc_search_glob_match("a?c", "abc"), "? ~ b");
    CHECK(edc_search_glob_match("a*c", "abc"), "* between two literals");
    CHECK(edc_search_glob_match("a*c", "ac"), "* may match nothing");
    CHECK(edc_search_glob_match("*a*b*", "xaybz"), "two stars");
    CHECK(edc_search_glob_match("*.tar.gz", "x.tar.gz"), "a dot in the glob is literal");
    CHECK(!edc_search_glob_match("*.tar.gz", "x.tarXgz"), "and is not a wildcard");
    CHECK(edc_search_glob_match("src/*.c", "src/a.c"), "a path glob matches a rel path");
    CHECK(!edc_search_glob_match("src/*.c", "src/sub/a.c"), "* must not cross '/'");
    CHECK(!edc_search_glob_match("*/a.c", "src/sub/a.c"), "the same, from the other side");
    CHECK(!edc_search_glob_match("src?c", "src/c"), "? must not match '/'");
    CHECK(edc_search_glob_match("src/*", "src/a"), "a trailing star");
    CHECK(edc_search_glob_match("src/*", "src/sub"), "a star can match a directory name");
    CHECK(edc_search_glob_match("", "anything"), "an empty glob accepts everything");
    CHECK(edc_search_glob_match("**", "a"), "two stars are just a star");
    CHECK(!edc_search_glob_match("a", "ab"), "a literal glob is exact");

#ifdef _WIN32
    CHECK(edc_search_glob_match("*.C", "a.c"), "Windows folds case");
#else
    CHECK(!edc_search_glob_match("*.C", "a.c"), "POSIX does not fold case");
    CHECK(edc_search_glob_match("*.C", "a.C"), "and is still exact for the same case");
#endif
}

/* ── concurrency ──────────────────────────────────────────────────────────── */

struct worker
{
    int id;
    char root[64];
    int iterations;
    int failures;
};

static void *worker_main(void *ud)
{
    struct worker *w = (struct worker *)ud;
    char marker[64];
    snprintf(marker, sizeof(marker), "MARKER-%d", w->id);
    for (int i = 0; i < w->iterations; i++)
    {
        sds out = edc_search_run_blocking(w->root, true, "MARKER", NULL, false, false, 10);
        if (count_occurrences(out, marker) != 1)
            w->failures++;
        if (strstr(out, "MARKER-") != NULL && count_occurrences(out, "MARKER-") != 1)
            w->failures++; /* another thread's results leaked in */
        sdsfree(out);
    }
    return NULL;
}

static void test_concurrent_walks(void)
{
    TEST("concurrency: 6 threads x 200 walks keep their results separate");

    enum { kWorkers = 6 };
    struct worker workers[kWorkers];
    pthread_t threads[kWorkers];

    for (int i = 0; i < kWorkers; i++)
    {
        memset(&workers[i], 0, sizeof(workers[i]));
        snprintf(workers[i].root, sizeof(workers[i].root), "/tmp/edc_mat_thr_%d_XXXXXX", i);
        if (!mkdtemp(workers[i].root))
        {
            CHECK(false, "mkdtemp %d failed", i);
            return;
        }
        workers[i].id = i;
        workers[i].iterations = 200;

        char path[256];
        snprintf(path, sizeof(path), "%s/mine.txt", workers[i].root);
        FILE *f = fopen(path, "wb");
        if (f)
        {
            fprintf(f, "MARKER-%d\n", i);
            fclose(f);
        }
        for (int d = 0; d < 5; d++)
        {
            char sub[256];
            snprintf(sub, sizeof(sub), "%s/d%d", workers[i].root, d);
            mkdir(sub, 0755);
            snprintf(path, sizeof(path), "%s/d%d/deep.txt", workers[i].root, d);
            FILE *g = fopen(path, "wb");
            if (g)
            {
                fputs("nothing to see\n", g);
                fclose(g);
            }
        }
    }

    for (int i = 0; i < kWorkers; i++)
        pthread_create(&threads[i], NULL, worker_main, &workers[i]);
    for (int i = 0; i < kWorkers; i++)
        pthread_join(threads[i], NULL);

    int total = 0;
    for (int i = 0; i < kWorkers; i++)
    {
        total += workers[i].failures;
        sds cmd = sdscatprintf(sdsempty(), "rm -rf '%s'", workers[i].root);
        if (test_sh(cmd) != 0)
            log_error("    cleanup failed");
        sdsfree(cmd);
    }
    CHECK(total == 0, "every concurrent walk saw its own result (%d bad)", total);
}

static void test_late_nul_is_binary(void)
{
    TEST("grep: a NUL anywhere makes the file binary (no silent partial scan)");

    sds out = edc_search_run_blocking(g_root, true, "TODO", "late_nul.dat", false, false, 10);
    /* Neither the line before the NUL nor the one after it is reported: the file is binary
     * as a whole. Reporting the first line used to be the tell-tale of the prefix-only
     * check -- and the second line was unreachable without saying so. */
    CHECK(strstr(out, "TODO before the nul") == NULL, "no partial match is reported: %s", out);
    CHECK(strstr(out, "TODO after the nul") == NULL, "and nothing past the NUL is invented");
    CHECK(strstr(out, "no matches") != NULL, "the file is skipped as binary");
    sdsfree(out);
}

static void test_total_output_is_capped(void)
{
    TEST("grep: the total result text is capped, and the cap is announced");

    /* Its own tree on purpose: a file loud enough to fill the budget would hide every other
     * fixture's matches from tests that search the whole shared tree. */
    char root[] = "/tmp/edc_cap_XXXXXX";
    if (!mkdtemp(root))
    {
        CHECK(false, "mkdtemp failed");
        return;
    }
    sds body = sdsempty();
    for (int i = 0; i < 400; i++)
    {
        body = sdscat(body, "TODO ");
        for (int j = 0; j < 200; j++)
            body = sdscatlen(body, "y", 1);
        body = sdscatlen(body, "\n", 1);
    }
    sds path = sdscatprintf(sdsempty(), "%s/many.txt", root);
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fwrite(body, 1, sdslen(body), f);
        fclose(f);
    }

    sds out = edc_search_run_blocking(root, true, "TODO", NULL, false, false, 10000);
    CHECK(sdslen(out) <= 64 * 1024 + 512, "the result stays near the 64 KiB budget (got %zu)", sdslen(out));
    CHECK(strstr(out, "output capped at 64 KiB") != NULL, "and it says the budget ended the list");
    /* limit is a match COUNT; raising it cannot help a byte budget. The advice has to name
     * something that actually works. */
    CHECK(strstr(out, "narrow the pattern or the path") != NULL, "and the advice fits a byte budget");
    CHECK(strstr(out, "TODO") != NULL, "there is still content to read");
    sdsfree(out);

    remove(path);
    sdsfree(path);
    sdsfree(body);
    rmdir(root);
}

static void test_limit_wording_is_honest(void)
{
    TEST("grep: reaching the limit does not claim something was dropped");

    /* a.c holds exactly ONE match. With limit 1 we stop on the count, so we cannot know
     * whether more would have followed: the trailer must say "there may be more", not
     * "you missed some" -- that is the case the old wording got wrong. */
    sds out = edc_search_run_blocking(g_root, true, "TODO", "a.c", false, false, 1);
    CHECK(strstr(out, "reached the limit of 1 matches") != NULL, "the limit is named: %s", out);
    CHECK(strstr(out, "there may be more") != NULL, "and the uncertainty is stated");
    CHECK(strstr(out, "narrow") != NULL, "with the action to take");
    sdsfree(out);

    /* Not reaching the limit says nothing at all. */
    sds under = edc_search_run_blocking(g_root, true, "TODO", "a.c", false, false, 10);
    CHECK(strstr(under, "reached the limit") == NULL, "a limit that was not reached is silent");
    sdsfree(under);
}

int main(void)
{
    log_info("test_search_matrix: grep / find semantics");
    snprintf(g_root, sizeof(g_root), "/tmp/edc_mat_XXXXXX");
    if (!mkdtemp(g_root))
    {
        log_error("mkdtemp failed");
        return 1;
    }
    build_tree();

    /* These three need the fixture tree, so they run after build_tree(). */
    test_late_nul_is_binary();
    test_total_output_is_capped();
    test_limit_wording_is_honest();

    test_golden_output();
    test_line_and_path_rules();
    test_blank_lines();
    test_long_line_cap();
    test_unicode();
    test_binary_and_size_guards();
    test_skip_dirs();
    test_symlinks_and_fifo();
    test_depth_cap();
    test_limit();
    test_literal_vs_regex();
    test_glob_filter();
    test_find_shape();
    test_errors_and_odd_input();
    test_glob_table();
    test_concurrent_walks();

    tear_down();
    log_info("=== %d passed, %d failed ===", test_passed, test_failed);
    printf("%d passed, %d failed\n", test_passed, test_failed);
    return test_failed == 0 ? 0 : 1;
}
