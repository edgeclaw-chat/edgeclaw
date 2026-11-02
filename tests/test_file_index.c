/* tests/test_file_index.c -- the list behind `@` completion.
 *
 * What matters: it walks the tree ONCE and answers from memory, it never descends into
 * the directories nobody means to reference, directories come first, and a query narrows
 * the list instead of walking again.
 */
#include "edc_file_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int passed = 0, failed = 0;
#define TEST(n)                                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        printf("  TEST: %-58s ", n);                                                                                   \
    } while (0)
#define CHECK(c, m)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL: %s\n", m);                                                                                   \
            failed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            printf("PASS\n");                                                                                          \
            passed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f)
    {
        fputs("x\n", f);
        fclose(f);
    }
}

static int count_rows(const char *buf)
{
    int n = 0;
    for (const char *p = buf; *p; p++)
        if (*p == '\n')
            n++;
    return n;
}

int main(void)
{
    const char *ws = "/tmp/test_file_index_ws";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/sub/deep %s/.git %s/node_modules %s/build", ws, ws, ws, ws, ws);
    if (system(cmd) != 0)
        return 1;

    char p[512];
    snprintf(p, sizeof(p), "%s/a.txt", ws);
    touch(p);
    snprintf(p, sizeof(p), "%s/sub/b.txt", ws);
    touch(p);
    snprintf(p, sizeof(p), "%s/sub/deep/c.txt", ws);
    touch(p);
    snprintf(p, sizeof(p), "%s/.git/HEAD", ws);
    touch(p);
    snprintf(p, sizeof(p), "%s/node_modules/dep.js", ws);
    touch(p);
    snprintf(p, sizeof(p), "%s/build/out.o", ws);
    touch(p);

    edc_file_index_reset();
    char buf[4096];

    TEST("an empty query lists one level: directories first, noisy ones dropped");
    {
        const size_t rows = edc_file_index_query(ws, "", buf, sizeof(buf));
        CHECK(rows == 2, "sub/ and a.txt -- .git, node_modules and build are not offered");
        CHECK(strncmp(buf, "sub/", 4) == 0, "the directory comes first");
        CHECK(strstr(buf, ".git") == NULL && strstr(buf, "node_modules") == NULL && strstr(buf, "build/") == NULL,
              "the noisy directories are gone");
    }

    TEST("a bare name is a STRICT PREFIX (no fuzzy)");
    {
        CHECK(edc_file_index_query(ws, "a.t", buf, sizeof(buf)) == 1, "'a.t' finds a.txt");
        CHECK(edc_file_index_query(ws, "atx", buf, sizeof(buf)) == 0, "'atx' is not a prefix of anything");
        CHECK(edc_file_index_query(ws, "A.T", buf, sizeof(buf)) == 1, "case-insensitive");
    }

    TEST("a space in the last component is an ordinary byte (quoted references complete)");
    {
        /* Its own directory, created here: a name with a space would otherwise add a row to
         * the exact-list assertions above. `@"my file.txt"` has to be completable, and the
         * space must not be a separator here -- in a reference it is the quote's job. */
        char d[512];
        snprintf(d, sizeof(d), "%s/spaced", ws);
        mkdir(d, 0777);
        snprintf(p, sizeof(p), "%s/my file.txt", d);
        touch(p);
        const size_t n = edc_file_index_query(ws, "spaced/my fi", buf, sizeof(buf));
        CHECK(n == 1 && strstr(buf, "my file.txt") != NULL, "the row comes back with its space");
    }

    TEST("a typed prefix keeps the noisy directories available");
    {
        CHECK(edc_file_index_query(ws, "bu", buf, sizeof(buf)) == 1, "'bu' finds build/");
        CHECK(strstr(buf, "build/") != NULL, "even though a bare listing hides it");
    }

    TEST("a path prefix browses one level");
    {
        /* The directory the query points INTO comes first (its own row), then its
         * children -- `@sub` is a real reference (the mention pass lists a directory),
         * so the popup must be able to hand it back. */
        CHECK(edc_file_index_query(ws, "sub/", buf, sizeof(buf)) == 3, "sub itself, b.txt, deep/");
        CHECK(strncmp(buf, "sub\ts\n", 6) == 0, "the directory itself is kind 's', ahead of its children");
        CHECK(strstr(buf, "sub/b.txt\tf") != NULL, "a file child keeps its path and kind");
        CHECK(strstr(buf, "sub/deep/\td") != NULL, "the nested directory is written with its path");
        CHECK(edc_file_index_query(ws, "sub/de", buf, sizeof(buf)) == 1, "one more component: only deep/ is one level in");
        CHECK(strstr(buf, "sub/deep/\td") != NULL && strstr(buf, "\ts\n") == NULL,
              "a partial name has no 'the directory itself' row");
    }

    TEST(".. walks out of the root, and the rows become absolute");
    {
        CHECK(edc_file_index_query(ws, "../", buf, sizeof(buf)) == 0 || buf[0] == '/',
              "either nothing, or absolute paths (never a bare name that means nothing)");
    }

    TEST("~ is the home directory");
    {
        const size_t rows = edc_file_index_query(ws, "~/", buf, sizeof(buf));
        CHECK(rows > 0, "the home directory has entries");
        CHECK(buf[0] == '/', "`~` is expanded, so the rows are absolute paths");
        CHECK(edc_file_index_query(ws, "~nosuchuser/", buf, sizeof(buf)) == 0, "'~user' is not supported");
    }

    TEST("an absolute query browses the filesystem");
    {
        char abs_query[512];
        snprintf(abs_query, sizeof(abs_query), "%s/sub/", ws);
        CHECK(edc_file_index_query("/nonexistent-root", abs_query, buf, sizeof(buf)) == 2, "one level of children");
        CHECK(strstr(buf, "/sub/b.txt") != NULL, "rows carry absolute paths");

        snprintf(abs_query, sizeof(abs_query), "%s/su", ws);
        CHECK(edc_file_index_query("/nonexistent-root", abs_query, buf, sizeof(buf)) == 1, "the last component is a prefix");
        CHECK(edc_file_index_query("/nonexistent-root", "/", buf, sizeof(buf)) > 0, "'/' lists the root");
    }

    TEST("a directory that does not exist answers nothing");
    {
        CHECK(edc_file_index_query("/tmp/test_file_index_missing", "", buf, sizeof(buf)) == 0, "0 rows");
        CHECK(edc_file_index_query(ws, "nosuchdir/", buf, sizeof(buf)) == 0, "and so does a missing subdirectory");
    }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    if (system(cmd) != 0)
        printf("  (cleanup failed)\n");

    printf("\n  %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
