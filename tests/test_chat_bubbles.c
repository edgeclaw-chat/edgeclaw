/* test_chat_bubbles: the v9 durable view-state table.
 *
 * What is worth pinning down (each of these has a silent failure mode):
 *  - append/page round trip, including an encoded line with escaped \n / \t / \\:
 *    a store that "helpfully" decodes would corrupt tool output on replay;
 *  - paging is a CURSOR, not an offset: no duplicates, no gaps, ascending order;
 *  - opening a v8 database migrates to v9 with the old rows untouched
 *    (test_state_migrate covers the runner; here we cover the table's own API
 *    against a freshly created store).
 */
#include "edc_config.h"
#include "edc_state_store.h"
#include "sds.h"
#include "test_fs_util.h"

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, ...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "    FAIL: ");                                                                                \
            fprintf(stderr, __VA_ARGS__);                                                                                 \
            fprintf(stderr, "\n");                                                                                         \
            g_failed++;                                                                                                    \
        }                                                                                                                  \
        else                                                                                                               \
        {                                                                                                                  \
            printf("    ok: ");                                                                                            \
            printf(__VA_ARGS__);                                                                                           \
            printf("\n");                                                                                                  \
            g_passed++;                                                                                                    \
        }                                                                                                                  \
    } while (0)

static char g_dir[512];

static void reset_dir(void)
{
    char cmd[4096];
    edc_state_store_close();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if (test_sh(cmd) != 0)
        fprintf(stderr, "    (warning: could not reset %s)\n", g_dir);
}

static struct edc_config *make_config(void)
{
    struct edc_config *cfg = calloc(1, sizeof(*cfg));
    cfg->workspace = sdsnew(g_dir);
    return cfg;
}

static void free_config(struct edc_config *cfg)
{
    if (!cfg)
        return;
    sdsfree(cfg->workspace);
    free(cfg);
}

static void append_n(const char *sid, int n)
{
    for (int i = 0; i < n; i++)
    {
        char line[128];
        snprintf(line, sizeof(line), "sys\t0\t\t\tbubble %d", i);
        int64_t seq = 0;
        if (!edc_chat_bubble_append(sid, line, &seq) || seq != i + 1)
        {
            fprintf(stderr, "    FAIL: append #%d failed (seq=%" PRId64 ")\n", i, seq);
            g_failed++;
            return;
        }
    }
}

static void test_append_and_page(void)
{
    printf("\n-- append / page round trip --\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    CHECK(edc_state_store_open(cfg), "store opens");

    int64_t seq = 0;
    /* Encoded transcript lines carry escaped newlines/tabs/backslashes; the DB
     * must store the line verbatim, without decoding it. */
    const char *tricky = "tool\\t0\\tt1\\theader\\tline one\\nline two\\twith a backslash \\\\ end";
    CHECK(edc_chat_bubble_append("s1", tricky, &seq) && seq == 1, "first append gets seq 1");
    CHECK(edc_chat_bubble_append("s1", "sys\\t0\\t\\t\\tbubble 1", &seq) && seq == 2,
          "second append gets seq 2");
    CHECK(edc_chat_bubble_append("s2", "sys\\t0\\t\\t\\tother session", &seq) && seq == 1,
          "seq is per session");

    struct edc_chat_bubble_row *rows = NULL;
    int n = 0;
    CHECK(edc_chat_bubble_page("s1", INT64_MAX, 10, &rows, &n), "page query succeeds");
    CHECK(n == 2, "s1 has 2 rows, got %d", n);
    if (n == 2)
    {
        CHECK(rows[0].seq == 1 && strcmp(rows[0].line, tricky) == 0,
              "the escaped line is stored byte-for-byte");
        CHECK(rows[1].seq == 2, "rows are in ascending seq order");
    }
    edc_chat_bubble_rows_free(rows, n);

    rows = NULL;
    n = 0;
    CHECK(edc_chat_bubble_page("s2", INT64_MAX, 10, &rows, &n) && n == 1,
          "another session's page is independent");
    edc_chat_bubble_rows_free(rows, n);

    CHECK(edc_chat_bubble_count("s1") == 2, "count(s1) == 2");
    CHECK(edc_chat_bubble_count("s2") == 1, "count(s2) == 1");
    CHECK(edc_chat_bubble_count("s3") == 0, "count(unknown) == 0");
    free_config(cfg);
}

static void test_paging_boundaries(void)
{
    printf("\n-- paging boundaries --\n");
    reset_dir();
    struct edc_config *cfg = make_config();
    CHECK(edc_state_store_open(cfg), "store opens");

    append_n("big", 25);
    struct edc_chat_bubble_row *rows = NULL;
    int n = 0;
    CHECK(edc_chat_bubble_page("big", INT64_MAX, 10, &rows, &n) && n == 10,
          "newest page has exactly 10 rows, got %d", n);
    if (n == 10)
        CHECK(rows[0].seq == 16 && rows[9].seq == 25,
              "newest page is seq 16..25 in ascending order (got %" PRId64 "..%" PRId64 ")",
              rows[0].seq, rows[9].seq);
    int64_t cursor = n ? rows[0].seq : 0;
    edc_chat_bubble_rows_free(rows, n);

    rows = NULL;
    n = 0;
    CHECK(edc_chat_bubble_page("big", cursor, 10, &rows, &n) && n == 10,
          "second page has 10 rows, got %d", n);
    if (n == 10)
        CHECK(rows[0].seq == 6 && rows[9].seq == 15,
              "second page is seq 6..15, no overlap with the first");
    cursor = n ? rows[0].seq : 0;
    edc_chat_bubble_rows_free(rows, n);

    rows = NULL;
    n = 0;
    CHECK(edc_chat_bubble_page("big", cursor, 10, &rows, &n) && n == 5,
          "last page has the remaining 5 rows, got %d", n);
    if (n == 5)
        CHECK(rows[0].seq == 1 && rows[4].seq == 5, "last page is seq 1..5");
    cursor = n ? rows[0].seq : 0;
    edc_chat_bubble_rows_free(rows, n);

    rows = NULL;
    n = 0;
    CHECK(edc_chat_bubble_page("big", cursor, 10, &rows, &n) && n == 0 && rows == NULL,
          "an empty page is a valid result, not an error");
    edc_chat_bubble_rows_free(rows, n);

    rows = NULL;
    n = 0;
    CHECK(!edc_chat_bubble_page("big", INT64_MAX, 0, &rows, &n), "limit 0 is refused");
    CHECK(!edc_chat_bubble_page("", INT64_MAX, 10, &rows, &n), "empty session id is refused");
    free_config(cfg);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/edc_chat_bubbles_test_XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(g_dir))
    {
        fprintf(stderr, "cannot create temp dir\n");
        return 2;
    }
    /* mkdtemp made the directory; reset_dir() will reuse the path. */
    test_append_and_page();
    test_paging_boundaries();
    edc_state_store_close();
    {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
        test_sh(cmd);
    }
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
