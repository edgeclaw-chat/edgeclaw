#ifndef EDC_UTF8_H
#define EDC_UTF8_H

#include <stdbool.h>
#include <stddef.h>

/* Strict UTF-8 validation, header-only on purpose.
 *
 * "Strict" is the whole point here. A lenient decoder also "accepts" a GBK line as some
 * sequence of Latin-1-ish characters -- and then the transcoding at the shell boundary, whose
 * only job is to turn that GBK line into real UTF-8, could never fire. So overlong forms
 * (C0 80), UTF-16 surrogates (ED A0 80) and code points above U+10FFFF (F4 90 80 80) are all
 * rejected.
 *
 * Inline rather than a library call because its two users sit on opposite sides of the build
 * graph -- the platform layer and the tests -- and pulling a library in for thirty lines would
 * drag dependencies (mbedtls, through edc_string_utils) into edc_platform, which is
 * deliberately standalone. */

/* Length (1..4) of the well-formed sequence starting at |p|, or 0 if the bytes there are not
 * one. |remaining| is how many bytes are readable from |p|. */
static inline size_t edc_utf8_seq_len(const unsigned char *p, size_t remaining)
{
    if (remaining == 0)
        return 0;

    const unsigned char c = p[0];
    if (c < 0x80)
        return 1;

    size_t extra; /* continuation bytes after the lead */
    unsigned int cp;
    if (c >= 0xC2 && c <= 0xDF)
    {
        extra = 1;
        cp = c & 0x1Fu;
    }
    else if (c >= 0xE0 && c <= 0xEF)
    {
        extra = 2;
        cp = c & 0x0Fu;
    }
    else if (c >= 0xF0 && c <= 0xF4)
    {
        extra = 3;
        cp = c & 0x07u;
    }
    else
    {
        /* 80..BF: a continuation byte with no lead. C0/C1: an overlong 2-byte lead.
         * F5..FF: above U+10FFFF. None of them starts a sequence. */
        return 0;
    }

    if (remaining < extra + 1)
        return 0; /* truncated */

    for (size_t i = 1; i <= extra; i++)
    {
        if ((p[i] & 0xC0u) != 0x80u)
            return 0;
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }

    /* What the length class alone still allows: a 3-byte form spelling something below 0x800,
     * a 4-byte form below 0x10000, a surrogate half, or a value past the last code point. */
    if ((extra == 2 && cp < 0x800u) || (extra == 3 && cp < 0x10000u) || cp > 0x10FFFFu ||
        (cp >= 0xD800u && cp <= 0xDFFFu))
        return 0;

    return extra + 1;
}

/* TRUE if every one of |len| bytes is part of a well-formed UTF-8 sequence. */
static inline bool edc_utf8_is_valid(const char *str, size_t len)
{
    const unsigned char *p = (const unsigned char *)str;
    size_t i = 0;
    while (i < len)
    {
        size_t n = edc_utf8_seq_len(p + i, len - i);
        if (n == 0)
            return false;
        i += n;
    }
    return true;
}

#endif /* EDC_UTF8_H */
