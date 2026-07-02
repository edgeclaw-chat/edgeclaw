#include "edc_datetime_utils.h"

#include <time.h>

sds edc_datetime_timestamp(void)
{
    time_t t = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return sdsnew(buf);
}
