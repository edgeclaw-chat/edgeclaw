/* Deny-list sanity: an entry that does not resolve matches nothing, so the directory it
 * names stays readable. That is a silent hole, so edc_sandbox_init() reports it -- this
 * pins the reporting (and the fact that a good entry is not reported). */
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_sandbox.h"
#include "sds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_fs_util.h"

/* Per-run scratch directory, created under Bazel's per-test TEST_TMPDIR. These cases used a
 * FIXED "/tmp/test_..." path: that both assumes /tmp exists (POSIX-only -- on Windows it means
 * "\tmp" on the current drive) and makes two runs of the binary share one tree, which leaves
 * state behind when a run is killed. */
static char g_scratch[1024];

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

int main(void)
{
    test_tmp_template(g_scratch, sizeof(g_scratch), "scratch");
    if (!mkdtemp(g_scratch))
    {
        perror("mkdtemp");
        return 1;
    }
    const char *dir = g_scratch;
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    if (test_sh(cmd) != 0)
        return 1;

    struct edc_config conf;
    memset(&conf, 0, sizeof(conf));
    conf.enable_sandbox = true;

    TEST("no deny list: nothing to report");
    CHECK(edc_sandbox_report_unresolved_paths(&conf) == 0, "0");

    TEST("sandbox off: the lists are not in force, so nothing is reported");
    sds deny[2] = {sdsnew("/tmp/definitely_not_here_xyz"), NULL};
    conf.sandbox_deny_paths = deny;
    conf.enable_sandbox = false;
    CHECK(edc_sandbox_report_unresolved_paths(&conf) == 0, "0");

    TEST("an entry that cannot be resolved is reported (it masks nothing)");
    conf.enable_sandbox = true;
    CHECK(edc_sandbox_report_unresolved_paths(&conf) == 1, "1 bad entry");

    TEST("a resolvable entry is not reported");
    sdsfree(deny[0]);
    deny[0] = sdsnew(dir);
    CHECK(edc_sandbox_report_unresolved_paths(&conf) == 0, "0");

    TEST("one good and one bad: only the bad one counts");
    sds both[3] = {sdsnew(dir), sdsnew("/tmp/definitely_not_here_xyz"), NULL};
    sdsfree(deny[0]);
    conf.sandbox_deny_paths = both;
    CHECK(edc_sandbox_report_unresolved_paths(&conf) == 1, "1");

    TEST("NULL conf / empty list");
    CHECK(edc_sandbox_report_unresolved_paths(NULL) == 0, "NULL conf");
    sdsfree(both[0]);
    sdsfree(both[1]);
    conf.sandbox_deny_paths = NULL;
    CHECK(edc_sandbox_report_unresolved_paths(&conf) == 0, "NULL list");

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (test_sh(cmd) != 0)
        printf("  (cleanup failed)\n");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
