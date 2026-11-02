/* tests/test_utf8_valid.c -- the strict UTF-8 validator (src/common/edc_utf8.h).
 *
 * Why its own test: the validator is the gate that decides whether a line of command output
 * gets transcoded from the ANSI code page or passed through. LENIENT is the failure mode that
 * matters -- a decoder that accepts overlong forms or surrogates also "accepts" a line of GBK,
 * and then the transcoding could never fire, so every Chinese line would stay mojibake. Each
 * rejected case below is one byte pattern that a lenient decoder would wave through.
 *
 * Runs on every platform: the header is pure C, and the same code is exercised on Windows. */

#include "edc_utf8.h"

#include <stdio.h>
#include <string.h>

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

int main(void)
{
    /* ── Accepted: the shapes real output is made of ───────────────────────── */
    CHECK(edc_utf8_is_valid("", 0), "empty is valid");
    CHECK(edc_utf8_is_valid("hello-world\n", 12), "ASCII is valid");
    CHECK(edc_utf8_is_valid("\xE4\xB8\xAD", 3), "3-byte CJK (U+4E2D) is valid");
    CHECK(edc_utf8_is_valid("\xC2\x80", 2), "the lowest 2-byte form (U+0080) is valid");
    CHECK(edc_utf8_is_valid("\xDF\xBF", 2), "the highest 2-byte form (U+07FF) is valid");
    CHECK(edc_utf8_is_valid("\xE0\xA0\x80", 3), "the lowest 3-byte form (U+0800) is valid");
    CHECK(edc_utf8_is_valid("\xF0\x9F\x98\x80", 4), "a 4-byte emoji (U+1F600) is valid");
    CHECK(edc_utf8_is_valid("\xEF\xBB\xBF", 3), "a BOM alone IS a well-formed code point (U+FEFF)");
    CHECK(edc_utf8_is_valid("\xE4\xB8\xAD\n\xE6\x96\x87\n", 8), "several lines, no truncation");

    /* ── Rejected: one lenient-decoder loophole each ───────────────────────── */
    CHECK(!edc_utf8_is_valid("\x80", 1), "a lone continuation byte");
    CHECK(!edc_utf8_is_valid("\xBF", 1), "a lone continuation byte, high end");
    CHECK(!edc_utf8_is_valid("\xC0\x80", 2), "overlong 2-byte form of NUL");
    CHECK(!edc_utf8_is_valid("\xC1\xBF", 2), "overlong 2-byte form (lead C1)");
    CHECK(!edc_utf8_is_valid("\xE0\x80\x80", 3), "overlong 3-byte form");
    CHECK(!edc_utf8_is_valid("\xF0\x80\x80\x80", 4), "overlong 4-byte form");
    CHECK(!edc_utf8_is_valid("\xED\xA0\x80", 3), "a UTF-16 surrogate half (U+D800) is not UTF-8");
    CHECK(!edc_utf8_is_valid("\xED\xBF\xBF", 3), "the top surrogate half (U+DFFF)");
    CHECK(!edc_utf8_is_valid("\xF4\x90\x80\x80", 4), "above U+10FFFF");
    CHECK(!edc_utf8_is_valid("\xF5\x80\x80\x80", 4), "lead F5 does not start a sequence");
    CHECK(!edc_utf8_is_valid("\xFF", 1), "no sequence starts with FF");
    CHECK(!edc_utf8_is_valid("\xE4\xB8", 2), "a truncated 3-byte sequence");
    CHECK(!edc_utf8_is_valid("\xC2", 1), "a truncated 2-byte sequence");
    CHECK(!edc_utf8_is_valid("\xE4\xB8\xAD\xE6", 4), "valid text with a truncated tail");
    CHECK(!edc_utf8_is_valid("\xC2\x41", 2), "a continuation byte that is not one");
    CHECK(!edc_utf8_is_valid("\xD6\xD0\xCE\xC4", 4), "a line of GBK ('\xD6\xD0\xCE\xC4') is not UTF-8");

    /* ── seq_len: the length the repair path copies whole ───────────────────── */
    CHECK(edc_utf8_seq_len((const unsigned char *)"a", 1) == 1, "ASCII is one byte");
    CHECK(edc_utf8_seq_len((const unsigned char *)"\xE4\xB8\xAD", 3) == 3, "CJK is three bytes");
    CHECK(edc_utf8_seq_len((const unsigned char *)"\xF0\x9F\x98\x80", 4) == 4, "emoji is four bytes");
    CHECK(edc_utf8_seq_len((const unsigned char *)"\x80", 1) == 0, "no length for a stray byte");
    CHECK(edc_utf8_seq_len((const unsigned char *)"\xE4\xB8", 2) == 0, "no length for a truncated one");

    /* Length-bounded, not NUL-bounded: an embedded NUL must not end the scan early, or a
     * buffer of binary output would be judged only by its first segment. */
    CHECK(edc_utf8_is_valid("ab\0cd", 5), "an embedded NUL is a valid code point");
    CHECK(!edc_utf8_is_valid("ab\0\xD6\xD0", 5), "and does not hide an invalid byte after it");

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
