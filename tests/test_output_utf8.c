/* tests/test_output_utf8.c -- the boundary conversion (edc_platform_output_to_utf8).
 *
 * This is the one place that may rewrite a command's bytes, so the test states the two halves
 * separately:
 *   - Windows: a line that is not UTF-8 is transcoded from the ANSI code page, one line at a
 *     time (a single command really does emit both encodings), and a line that is legal in
 *     neither ends up as U+FFFD rather than as plausible-looking garbage;
 *   - POSIX: nothing happens at all -- not even validation. Bytes in are bytes out, which is
 *     what `cat` of a binary file and a legacy GBK tool both rely on.
 *
 * The byte strings are spelled in hex so the test does not depend on the encoding of its own
 * source file. */

#include "edc_platform.h"
#include "edc_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* CP_ACP / MultiByteToWideChar, used ONLY to ask the operating system which of the two
 * branches below this code page takes: the product makes the same call, and a test that
 * guessed instead would encode the guess rather than the contract. */
#include <windows.h>
#endif

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

/* "\xD6\xD0\xCE\xC4\n" is 中文 (two characters) in GBK; the same text in UTF-8 is three bytes
 * per character. Both keep the trailing newline, because a line keeps its terminator. */
static const char k_gbk_cn[] = "\xD6\xD0\xCE\xC4\n";
static const char k_utf8_cn[] = "\xE4\xB8\xAD\xE6\x96\x87\n";
#define GBK_CN_LEN (sizeof(k_gbk_cn) - 1)
#define UTF8_CN_LEN (sizeof(k_utf8_cn) - 1)

int main(void)
{
    size_t len = 0;
    size_t converted = 0;

    /* ── Both platforms: the "nothing to do" answers must be free ─────────── */
    CHECK(edc_platform_output_to_utf8(NULL, 0, &len, &converted) == NULL, "no buffer: nothing to convert");
    CHECK(edc_platform_output_to_utf8("", 0, &len, &converted) == NULL, "empty output: nothing to convert");
    CHECK(edc_platform_output_to_utf8("hello\nworld\n", 12, &len, &converted) == NULL,
          "pure ASCII: nothing to convert");
    CHECK(edc_platform_output_to_utf8(k_utf8_cn, UTF8_CN_LEN, &len, &converted) == NULL,
          "already valid UTF-8: nothing to convert, and no rewritten copy");
    CHECK(converted == 0, "...and no line is reported as converted");

#if defined(_WIN32)
    const bool expect_bom_metadata = true; /* Set-Content / Out-File / editors all write one */
#else
    const bool expect_bom_metadata = false; /* a BOM here is just bytes */
#endif
    CHECK(edc_platform_bom_is_metadata() == expect_bom_metadata, "a leading BOM is metadata on Windows only");

#if defined(_WIN32)
    const unsigned int cp = edc_platform_ansi_codepage();
    CHECK(cp != 0, "Windows has an ANSI code page to name");

    /* A GBK line becomes the same text in UTF-8. Byte-exact only where GBK IS the ANSI code
     * page: on a 1252 or a UTF-8-default machine the same bytes are some other legal text, and
     * the contract is only "valid UTF-8, one line converted". */
    {
        char *out = edc_platform_output_to_utf8(k_gbk_cn, GBK_CN_LEN, &len, &converted);
        CHECK(out != NULL, "a line that is not UTF-8 is converted");
        if (out)
        {
            CHECK(converted == 1, "one line was converted");
            CHECK(edc_utf8_is_valid(out, len), "the result is valid UTF-8");
            if (cp == 936)
            {
                CHECK(len == UTF8_CN_LEN, "two GBK characters become six bytes of UTF-8, newline kept");
                CHECK(memcmp(out, k_utf8_cn, UTF8_CN_LEN) == 0, "and it is the same text");
            }
            free(out);
        }
    }

    /* A line with no trailing newline is still a line. */
    {
        char *out = edc_platform_output_to_utf8("\xD6\xD0", 2, &len, &converted);
        CHECK(out != NULL, "the last line needs no newline to be converted");
        CHECK(edc_utf8_is_valid(out, len), "...and is valid UTF-8");
        if (out && cp == 936)
            CHECK(len == 3 && memcmp(out, "\xE4\xB8\xAD", 3) == 0, "...and is the right character");
        free(out);
    }

    /* The mixed case: ONE command emitting both encodings in one invocation (PowerShell's own
     * text, then a python grandchild). A whole-buffer decision would corrupt the first line;
     * per line, each gets what it needs. */
    {
        char buf[64];
        size_t n = 0;
        memcpy(buf + n, k_utf8_cn, UTF8_CN_LEN); /* line 1: already UTF-8 */
        n += UTF8_CN_LEN;
        memcpy(buf + n, k_gbk_cn, GBK_CN_LEN); /* line 2: GBK */
        n += GBK_CN_LEN;

        char *out = edc_platform_output_to_utf8(buf, n, &len, &converted);
        CHECK(out != NULL, "a mixed buffer is converted");
        if (out)
        {
            CHECK(converted == 1, "exactly the one line that was not UTF-8 is converted");
            CHECK(edc_utf8_is_valid(out, len), "the whole result is valid UTF-8");
            CHECK(memcmp(out, k_utf8_cn, UTF8_CN_LEN) == 0, "the UTF-8 line is UNTOUCHED");
            if (cp == 936)
                CHECK(memcmp(out + UTF8_CN_LEN, k_utf8_cn, UTF8_CN_LEN) == 0, "and the GBK line became the same text");
            free(out);
        }
    }

    /* Legal in neither: the repair path. The point is that the damage is visible (U+FFFD) and
     * the result is still valid UTF-8 -- not that some plausible text was invented.
     *
     * WHICH inputs are "legal in neither" depends on the ANSI code page, which is why this
     * asserts conditionally. A single-byte page maps all 256 byte values, so nothing in this
     * range is illegal and the repair path cannot be reached; a double-byte page rejects
     * plenty. Assuming one case is what made this pass on a GBK machine and fail on a US
     * runner with CP437. The condition is asked of the OS rather than guessed: MultiByteToWideChar
     * with MB_ERR_INVALID_CHARS is the same call the implementation makes, so the test pins
     * the contract ("refused bytes become U+FFFD, accepted bytes do not") instead of pinning
     * one code page. */
    {
        static const char k_refused[] = "\xFF\xFE\n";
        const bool ansi_refuses =
            MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, k_refused, 3, NULL, 0) <= 0;

        char *out = edc_platform_output_to_utf8(k_refused, 3, &len, &converted);
        CHECK(out != NULL, "a line that is not UTF-8 is repaired, not dropped");
        if (out)
        {
            CHECK(edc_utf8_is_valid(out, len), "the repaired line is valid UTF-8");
            if (ansi_refuses)
                CHECK(memchr(out, '\xEF', len) != NULL,
                      "and carries U+FFFD where the sequence should have been");
            else
                CHECK(memchr(out, '\xEF', len) == NULL,
                      "this code page accepts those bytes, so there was nothing to repair");
            CHECK(converted == 1, "and counts as converted");
            free(out);
        }
    }
#else
    /* POSIX: the bytes a command writes are the bytes the caller wants. Nothing is converted
     * and -- just as important -- nothing is validated: a `cat` of a binary file, or a legacy
     * tool that still emits GBK, keeps returning those exact bytes. */
    CHECK(edc_platform_ansi_codepage() == 0, "no code page to name on this platform");
    CHECK(edc_platform_output_to_utf8(k_gbk_cn, GBK_CN_LEN, &len, &converted) == NULL,
          "POSIX converts nothing, even for bytes that are not UTF-8");
    CHECK(converted == 0, "...and reports no conversion");
#endif

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
