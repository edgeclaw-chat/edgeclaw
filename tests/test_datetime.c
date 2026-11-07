/* test_datetime: the wall-clock helpers in src/common/edc_datetime_utils.c.
 *
 * These had no test of their own -- coverage came only from the callers that
 * happen to use them -- and every failure mode here is silent:
 *
 *  1. Unit mix-up. edc_datetime_now_ms() feeds message ids. If it ever returned
 *     seconds, ids would still look well formed ("m1700000000_3") and would
 *     only collide when two messages arrived inside the same second. Pinning it
 *     against time(NULL) * 1000 catches the 1000x error.
 *
 *  2. A clock that does not tick. An id needs the millisecond field to advance;
 *     a fixed value would make the sequence number the only discriminator.
 *
 *  3. format()/parse() disagreeing. Both work in local time, so the only honest
 *     test is a round trip -- a literal like "2023-06-15 12:00:00" would encode
 *     the build machine's timezone. Every timestamp below goes
 *     format -> parse -> format and must come back byte-identical, in any TZ.
 *
 * 0 is a sentinel meaning "unknown" in this API, never 1970-01-01, so that
 * gets its own case.
 */

#include "edc_datetime_utils.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "    FAIL: %s\n", msg); g_failed++; } \
    else { printf("    ok: %s\n", msg); g_passed++; } \
} while (0)

/* format -> parse -> format. 1 if the string survives unchanged.
 *
 * Timezone-independent by construction: both halves use the same local-time
 * convention (see the header), and a wall clock that falls in a DST fold maps
 * back to itself, so no assertion below depends on the machine's TZ. */
static int roundtrip_ok(const char *s)
{
    time_t t = 0;
    if (!edc_datetime_parse(s, &t))
        return 0;
    sds back = edc_datetime_format(t);
    int ok = (strcmp(back, s) == 0);
    sdsfree(back);
    return ok;
}

int main(void)
{
    printf("\n── edc_datetime_now_ms returns milliseconds, not seconds ──\n");
    {
        time_t now = time(NULL);
        int64_t ms = edc_datetime_now_ms();
        int64_t expect = (int64_t)now * 1000;
        int64_t delta = ms - expect;
        if (delta < 0)
            delta = -delta;

        /* Generous window: the two calls straddle whatever else the machine is
         * doing. A unit mix-up is off by ~1000x, nowhere near this. */
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "within 5s of time(NULL)*1000 (delta=%" PRId64 " ms)", delta);
        CHECK(delta < 5000, buf);
    }

    printf("\n── the millisecond clock never runs backwards ──\n");
    {
        int ok = 1;
        int64_t prev = edc_datetime_now_ms();
        for (int i = 0; i < 10000; i++)
        {
            int64_t cur = edc_datetime_now_ms();
            if (cur < prev)
            {
                ok = 0;
                break;
            }
            prev = cur;
        }
        CHECK(ok, "10000 consecutive calls are non-decreasing");
    }

    printf("\n── and it actually advances ──\n");
    {
        /* Bounded spin rather than a sleep: waiting 2ms would make the test
         * take that long for a property that only needs the clock to tick once.
         * The cap is a few million quick calls (well under a second); hitting
         * it would itself be the failure -- a clock frozen inside a millisecond
         * would starve the id's timestamp field. */
        int64_t start = edc_datetime_now_ms();
        int advanced = 0;
        for (long i = 0; i < 10000000L; i++)
        {
            if (edc_datetime_now_ms() != start)
            {
                advanced = 1;
                break;
            }
        }
        CHECK(advanced, "the clock ticks at millisecond resolution");
    }

    printf("\n── edc_datetime_now is bracketed by time(NULL) ──\n");
    {
        time_t before = time(NULL);
        time_t got = edc_datetime_now();
        time_t after = time(NULL);
        CHECK(got >= before && got <= after,
              "edc_datetime_now() falls between two time(NULL) calls");
    }

    printf("\n── 0 means unknown, never 1970 ──\n");
    {
        sds a = edc_datetime_format(0);
        sds b = edc_datetime_format_min(0);
        CHECK(a && strcmp(a, "unknown") == 0, "edc_datetime_format(0) -> \"unknown\"");
        CHECK(b && strcmp(b, "unknown") == 0, "edc_datetime_format_min(0) -> \"unknown\"");
        sdsfree(a);
        sdsfree(b);
    }

    printf("\n── format/parse round trips (timezone-independent) ──\n");
    {
        CHECK(roundtrip_ok("2023-06-15 12:00:00"), "ordinary timestamp round-trips");
        CHECK(roundtrip_ok("2024-02-29 00:00:01"), "leap day round-trips");
        CHECK(roundtrip_ok("1980-01-01 00:00:01"), "an early but post-epoch date round-trips");
    }

    printf("\n── the formatted shapes are fixed-width ──\n");
    {
        time_t t = 0;
        CHECK(edc_datetime_parse("2023-06-15 12:34:56", &t), "reference timestamp parses");

        sds f = edc_datetime_format(t);
        CHECK(f && strlen(f) == 19, "edc_datetime_format is 19 chars (%Y-%m-%d %H:%M:%S)");

        sds m = edc_datetime_format_min(t);
        CHECK(m && strcmp(m, "2023-06-15 12:34") == 0,
              "edc_datetime_format_min -> %Y-%m-%d %H:%M");

        sds ts = edc_datetime_timestamp();
        CHECK(ts && strlen(ts) == 19, "edc_datetime_timestamp has the same 19-char shape");

        sdsfree(f);
        sdsfree(m);
        sdsfree(ts);
    }

    printf("\n── edc_datetime_parse rejects what it cannot read ──\n");
    {
        time_t t = 12345;
        CHECK(!edc_datetime_parse(NULL, &t), "NULL string is rejected");
        CHECK(!edc_datetime_parse("", &t), "empty string is rejected");
        CHECK(!edc_datetime_parse("2023-06-15", &t), "a date without a time is rejected");
        CHECK(!edc_datetime_parse("2023-06-15 12:34", &t), "missing seconds is rejected");
        CHECK(!edc_datetime_parse("not-a-date", &t), "garbage is rejected");
        CHECK(t == 12345, "a rejected parse leaves the out param untouched");
    }

    printf("\n── a NULL out param is allowed (validation-only call) ──\n");
    {
        CHECK(edc_datetime_parse("2023-06-15 12:00:00", NULL),
              "edc_datetime_parse(s, NULL) validates without writing");
    }

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
