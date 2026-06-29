#ifndef EDC_STRING_H
#define EDC_STRING_H

#include "sds.h"

static inline sds edc_str_new(const char *str)
{
    return sdsnew(str ? str : "");
}

static inline void edc_str_free(sds str)
{
    sdsfree(str);
}

#endif /* EDC_STRING_H */
