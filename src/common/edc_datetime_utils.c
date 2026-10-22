#include "edc_datetime_utils.h"
#include "edc_compat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static sds format_tm(time_t t, const char *fmt)
{
    if (t == 0)
        return sdsnew("unknown");

    struct tm tm_buf = {0};
    edc_localtime_r(&t, &tm_buf);

    char buf[32] = {0};
    strftime(buf, sizeof(buf), fmt, &tm_buf);
    return sdsnew(buf);
}

time_t edc_datetime_now(void)
{
    return time(NULL);
}

sds edc_datetime_format(time_t t)
{
    return format_tm(t, "%Y-%m-%d %H:%M:%S");
}

sds edc_datetime_format_min(time_t t)
{
    return format_tm(t, "%Y-%m-%d %H:%M");
}

bool edc_datetime_parse(const char *s, time_t *out)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
        return false;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    time_t v = mktime(&tm);
    if (v == (time_t)-1)
        return false;
    if (out)
        *out = v;
    return true;
}

sds edc_datetime_timestamp(void)
{
    return edc_datetime_format(time(NULL));
}
