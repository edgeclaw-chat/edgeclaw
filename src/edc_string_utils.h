#ifndef EDC_STRING_UTILS_H
#define EDC_STRING_UTILS_H

#include <stdarg.h>
#include <stddef.h>
#include "sds.h"

char *edc_strdup_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

int edc_strcmp0(const char *str1, const char *str2);

int edc_str_equal(const void *v1, const void *v2);
int edc_direct_equal(const void *v1, const void *v2);

unsigned int edc_str_hash(const void *v);
unsigned int edc_direct_hash(const void *v);

int edc_str_has_prefix(const char *str, const char *prefix);

char *edc_strstrip(char *str);

int edc_strv_length(char **str_array);
char **edc_strsplit(const char *str, const char *delimiter, int max_tokens);
void edc_strfreev(char **str_array);

sds edc_str_utf8_make_valid(const char *str, ssize_t len);

sds edc_str_uri_escape(const char *str, size_t len);

sds edc_str_joinv(const char *separator, char **str_array);
sds edc_str_vprintf(const char *fmt, va_list ap);

sds edc_base64_encode(const unsigned char *data, size_t len);
sds edc_base64_decode(const char *b64_str, size_t *out_len);

#endif
