#ifndef EDC_STRING_H
#define EDC_STRING_H

#include "sds.h"

/*
 * Thin SDS wrappers for GString migration.
 * sds is already char*; mutations may realloc → MUST reassign.
 */

#define edc_str_new(s)             ((s) ? sdsnew(s) : sdsempty())
#define edc_str_new_len(s, n)      sdsnewlen((s), (n))
#define edc_str_free(s)            sdsfree(s)
#define edc_str_len(s)             sdslen(s)
#define edc_str_cat(s, v)          do { s = sdscat(s, v); } while (0)
#define edc_str_catn(s, v, n)      do { s = sdscatlen(s, v, n); } while (0)
#define edc_str_catc(s, c)         do { char _c_ = (c); s = sdscatlen(s, &_c_, 1); } while (0)
#define edc_str_printf(s, f, ...)  do { s = sdscatprintf(s, f, ##__VA_ARGS__); } while (0)
#define edc_str_cpy(s, v)          do { s = sdscpy(s, v); } while (0)
#define edc_str_trunc(s, n)        sdsrange(s, 0, n)
#define edc_str_setlen(s, n)       sdssetlen(s, n)

#endif /* EDC_STRING_H */
