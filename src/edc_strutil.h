#ifndef EDC_STRUTIL_H
#define EDC_STRUTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>

typedef void (*EdcDestroyNotify)(void *data);

#define EDC_INT_TO_POINTER(i)  ((void *)(intptr_t)(i))

char  *edc_strdup_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

int    edc_strcmp0(const char *str1, const char *str2);

unsigned int edc_str_hash(const void *s);
unsigned int edc_direct_hash(const void *p);

int  edc_str_equal(const void *a, const void *b);
int  edc_direct_equal(const void *a, const void *b);

bool   edc_str_has_prefix(const char *str, const char *prefix);
char  *edc_strstrip(char *str);

size_t edc_strv_length(char **strv);
void   edc_strfreev(char **strv);
char **edc_strsplit(const char *str, const char *delimiter, int max_tokens);

#endif /* EDC_STRUTIL_H */
