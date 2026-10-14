/* tests/test_output_buf.c -- the capped accumulator (src/toolbox/edc_output_buf.c).
 *
 * This is the piece that makes "a tool cannot let its input decide how much memory the
 * process uses" testable at all: the limit is pure data, so it needs no event loop, no
 * process and no toolbox. The runaway *policy* (which reads these numbers) is covered by
 * tests/test_tool_shell_loop.c. */

#include "edc_output_buf.h"

#include "log.h"

#include <stdio.h>
#include <string.h>

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

static void test_empty_and_untouched(void)
{
    TEST("fresh buffer: nothing kept, nothing counted, not truncated");
    struct edc_output_buf b;
    edc_output_buf_init(&b, 1024);

    CHECK(b.data == NULL, "no data allocated until something arrives");
    CHECK(b.total == 0, "total 0");
    CHECK(!b.truncated, "not truncated");
    CHECK(edc_output_buf_over(&b, 100) == false, "0 bytes is not over any positive limit");

    edc_output_buf_free(&b);
}

static void test_under_cap_is_kept_verbatim(void)
{
    TEST("under the cap: everything kept, total == kept, no note");
    struct edc_output_buf b;
    edc_output_buf_init(&b, 64);

    edc_output_buf_append(&b, "hello ", 6);
    edc_output_buf_append(&b, "world", 5);

    CHECK(b.data && strcmp(b.data, "hello world") == 0, "content kept verbatim");
    CHECK(b.total == 11, "total counts what arrived");
    CHECK(!b.truncated, "not truncated");

    char note[64];
    edc_output_buf_truncation_note(&b, note, sizeof(note)); /* must be empty here */
    CHECK(note[0] == '\0', "no note when nothing was dropped");
    CHECK(strcmp(b.data, "hello world") == 0, "and the note does not touch the buffer");

    edc_output_buf_free(&b);
}

static void test_exactly_at_cap(void)
{
    TEST("exactly the cap: nothing dropped, so no truncation");
    struct edc_output_buf b;
    edc_output_buf_init(&b, 8);

    edc_output_buf_append(&b, "12345678", 8);

    CHECK(b.data && sdslen(b.data) == 8, "8 bytes kept");
    CHECK(b.total == 8, "total 8");
    CHECK(!b.truncated, "filling it exactly is not truncation");
    CHECK(edc_output_buf_over(&b, 8) == false, "not OVER the limit at exactly the limit");

    edc_output_buf_free(&b);
}

static void test_over_cap_keeps_prefix_and_counts_everything(void)
{
    TEST("over the cap: only the prefix is kept, total keeps counting");
    struct edc_output_buf b;
    edc_output_buf_init(&b, 4);

    edc_output_buf_append(&b, "abcdefghij", 10); /* one call, well past the cap */

    CHECK(b.data && sdslen(b.data) == 4, "4 bytes kept");
    CHECK(memcmp(b.data, "abcd", 4) == 0, "the PREFIX, not the tail");
    CHECK(b.total == 10, "total counts every byte offered");
    CHECK(b.truncated, "truncated");

    /* And it stays bounded no matter how much more arrives. */
    for (int i = 0; i < 1000; i++)
        edc_output_buf_append(&b, "0123456789", 10);
    CHECK(sdslen(b.data) == 4, "still 4 bytes after 10 KB more");
    CHECK(b.total == 10 + 1000 * 10, "total kept up (10010)");
    CHECK(edc_output_buf_over(&b, 5000) == true, "and it can be seen as over a limit");

    edc_output_buf_free(&b);
}

static void test_partial_fill_within_one_append(void)
{
    TEST("a single append that straddles the cap keeps the part that fits");
    struct edc_output_buf b;
    edc_output_buf_init(&b, 10);

    edc_output_buf_append(&b, "1234", 4);
    edc_output_buf_append(&b, "56789012345", 11);

    CHECK(sdslen(b.data) == 10, "filled to the cap");
    CHECK(strcmp(b.data, "1234567890") == 0, "kept the fitting part, in order");
    CHECK(b.total == 15, "total counts all 15");
    CHECK(b.truncated, "truncated");

    edc_output_buf_free(&b);
}

static void test_cap_zero_keeps_nothing(void)
{
    TEST("cap 0 keeps nothing (fail safe, not 'unlimited')");
    struct edc_output_buf b;
    edc_output_buf_init(&b, 0);

    edc_output_buf_append(&b, "abc", 3);

    CHECK(b.data == NULL, "nothing kept");
    CHECK(b.total == 3, "but it was counted");
    CHECK(b.truncated, "and reported as dropped");

    edc_output_buf_free(&b);
}

static void test_note_reports_kept_and_total(void)
{
    TEST("the truncation note states what was kept and what there was");
    struct edc_output_buf b;
    edc_output_buf_init(&b, 1024);

    /* 48 KiB in 1 KiB pieces, cap 1 KiB: kept 1.0 KiB of 48.0 KiB. */
    for (int i = 0; i < 3072; i++)
        edc_output_buf_append(&b, "0123456789abcdef", 16);
    CHECK(b.total == 49152, "total is the real volume");

    char note[160];
    edc_output_buf_truncation_note(&b, note, sizeof(note));
    CHECK(strcmp(note, "[output truncated: kept the first 1.0 KiB of 48.0 KiB]") == 0,
          "note carries both numbers, human-readable: %s", note);
    CHECK(sdslen(b.data) == 1024, "and formatting it leaves the captured bytes alone");

    edc_output_buf_free(&b);
}

static void test_size_str(void)
{
    TEST("byte formatting");
    char buf[32];

    edc_output_buf_size_str(0, buf, sizeof(buf));
    CHECK(strcmp(buf, "0 B") == 0, "0 -> %s", buf);
    edc_output_buf_size_str(812, buf, sizeof(buf));
    CHECK(strcmp(buf, "812 B") == 0, "bytes -> %s", buf);
    edc_output_buf_size_str(1024, buf, sizeof(buf));
    CHECK(strcmp(buf, "1.0 KiB") == 0, "1 KiB -> %s", buf);
    edc_output_buf_size_str(1u << 20, buf, sizeof(buf));
    CHECK(strcmp(buf, "1.0 MiB") == 0, "1 MiB -> %s", buf);
    edc_output_buf_size_str((size_t)256 << 20, buf, sizeof(buf));
    CHECK(strcmp(buf, "256.0 MiB") == 0, "256 MiB -> %s", buf);
    edc_output_buf_size_str((size_t)3 << 30, buf, sizeof(buf));
    CHECK(strcmp(buf, "3.0 GiB") == 0, "3 GiB -> %s", buf);
    /* Just under a boundary must still read as the BIGGER unit, not "1024.0 KiB" (this is
     * the number a real 1 MiB capture produced). */
    edc_output_buf_size_str(1048570, buf, sizeof(buf));
    CHECK(strcmp(buf, "1.0 MiB") == 0, "1048570 B -> %s", buf);
    edc_output_buf_size_str(1023, buf, sizeof(buf));
    CHECK(strcmp(buf, "1023 B") == 0, "1023 B stays in bytes -> %s", buf);
}

int main(void)
{
    log_info("=== edc_output_buf tests ===");
    log_info("");

    test_empty_and_untouched();
    test_under_cap_is_kept_verbatim();
    test_exactly_at_cap();
    test_over_cap_keeps_prefix_and_counts_everything();
    test_partial_fill_within_one_append();
    test_cap_zero_keeps_nothing();
    test_note_reports_kept_and_total();
    test_size_str();

    log_info("");
    log_info("=== 测试结果: %d passed, %d failed ===", test_passed, test_failed);
    return test_failed > 0 ? 1 : 0;
}
