#include "edc_semver.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* One parsed version. |pre| points INTO the caller's string and is not NUL-terminated at the
 * '+' (build metadata is skipped by comparing only up to it), so the end is carried too. */
struct parsed
{
    long major, minor, patch;
    const char *pre; /* NULL when there is no pre-release suffix */
    size_t pre_len;
};

static bool parse_number(const char **p, long *out)
{
    const char *s = *p;
    if (!isdigit((unsigned char)*s))
        return false;
    long v = 0;
    /* Three digits is enough for any real component; the guard is against overflow on a
     * malformed string, not against a legitimate version. */
    int digits = 0;
    while (isdigit((unsigned char)*s))
    {
        if (++digits > 9)
            return false;
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = v;
    *p = s;
    return true;
}

static bool parse_version(const char *s, struct parsed *out)
{
    if (!s || !*s)
        return false;

    memset(out, 0, sizeof(*out));

    /* The leading "v" is this project's tag form; harmless to accept, and accepting it means
     * callers do not have to strip it in three places. */
    if (*s == 'v' || *s == 'V')
        s++;

    if (!parse_number(&s, &out->major))
        return false;
    if (*s != '.')
        return false;
    s++;
    if (!parse_number(&s, &out->minor))
        return false;
    if (*s != '.')
        return false;
    s++;
    if (!parse_number(&s, &out->patch))
        return false;

    if (*s == '-')
    {
        s++;
        out->pre = s;
        while (*s && *s != '+')
            s++;
        out->pre_len = (size_t)(s - out->pre);
        if (out->pre_len == 0)
            return false; /* "1.2.3-" is malformed */
    }

    /* Build metadata, if present, is skipped: it does not affect precedence. Anything else
     * after the version means we did not understand the string. */
    if (*s == '+')
    {
        s++;
        if (!*s)
            return false;
        while (*s)
            s++;
    }

    return *s == '\0';
}

/* Compare pre-release identifiers, semver §11. Both sides are known non-empty. */
static int compare_prerelease(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t ai = 0, bi = 0;
    while (ai < alen && bi < blen)
    {
        /* One dot-separated identifier at a time. */
        size_t ae = ai, be = bi;
        while (ae < alen && a[ae] != '.')
            ae++;
        while (be < blen && b[be] != '.')
            be++;

        const char *as = a + ai;
        const char *bs = b + bi;
        const size_t asl = ae - ai;
        const size_t bsl = be - bi;

        bool a_num = asl > 0, b_num = bsl > 0;
        for (size_t i = 0; i < asl && a_num; i++)
            if (!isdigit((unsigned char)as[i]))
                a_num = false;
        for (size_t i = 0; i < bsl && b_num; i++)
            if (!isdigit((unsigned char)bs[i]))
                b_num = false;

        int cmp;
        if (a_num && b_num)
        {
            /* Numeric identifiers compare by value, not by length: rc.10 > rc.9. Leading
             * zeros are not valid semver, so a plain length-then-bytewise comparison is
             * enough after stripping them. */
            size_t az = 0, bz = 0;
            while (az + 1 < asl && as[az] == '0')
                az++;
            while (bz + 1 < bsl && bs[bz] == '0')
                bz++;
            const size_t an = asl - az, bn = bsl - bz;
            if (an != bn)
                cmp = an < bn ? -1 : 1;
            else
                cmp = memcmp(as + az, bs + bz, an);
        }
        else if (a_num)
        {
            cmp = -1; /* numeric identifiers have lower precedence than alphanumeric */
        }
        else if (b_num)
        {
            cmp = 1;
        }
        else
        {
            const size_t n = asl < bsl ? asl : bsl;
            cmp = memcmp(as, bs, n);
            if (cmp == 0 && asl != bsl)
                cmp = asl < bsl ? -1 : 1;
        }
        if (cmp != 0)
            return cmp < 0 ? -1 : 1;

        ai = ae < alen ? ae + 1 : ae;
        bi = be < blen ? be + 1 : be;
    }

    /* A shorter identifier list is lower when everything compared equal: rc.1 < rc.1.1. */
    const bool a_more = ai < alen;
    const bool b_more = bi < blen;
    if (a_more == b_more)
        return 0;
    return a_more ? 1 : -1;
}

static int compare_parsed(const struct parsed *a, const struct parsed *b)
{
    if (a->major != b->major)
        return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor)
        return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch)
        return a->patch < b->patch ? -1 : 1;

    /* A release outranks any pre-release of the same number: 1.2.3-rc.1 < 1.2.3. */
    if (!a->pre && !b->pre)
        return 0;
    if (!a->pre)
        return 1;
    if (!b->pre)
        return -1;
    return compare_prerelease(a->pre, a->pre_len, b->pre, b->pre_len);
}

bool edc_semver_is_newer(const char *candidate, const char *current)
{
    struct parsed a, b;
    if (!parse_version(candidate, &a) || !parse_version(current, &b))
        return false;
    return compare_parsed(&a, &b) > 0;
}
