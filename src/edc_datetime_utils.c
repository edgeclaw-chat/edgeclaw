#include "edc_datetime_utils.h"

#include <time.h>

sds edc_datetime_timestamp(void)
{
    time_t t = time(NULL);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return sdsnew(buf);
}
