#include "edc_strutil.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *edc_strdup_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *result = NULL;
    if (vasprintf(&result, format, args) < 0)
        result = NULL;
    va_end(args);
    return result;
}

int edc_strcmp0(const char *str1, const char *str2)
{
    if (str1 == NULL && str2 == NULL) return 0;
    if (str1 == NULL) return -1;
    if (str2 == NULL) return 1;
    return strcmp(str1, str2);
}

int edc_str_equal(const void *a, const void *b)
{
    return edc_strcmp0((const char *)a, (const char *)b) == 0;
}

unsigned int edc_str_hash(const void *s)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned int hash = 5381;
    int c;

    while ((c = *p++) != 0)
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

unsigned int edc_direct_hash(const void *p)
{
    return (unsigned int)(uintptr_t)p;
}

int edc_direct_equal(const void *a, const void *b)
{
    return a == b;
}

bool edc_str_has_prefix(const char *str, const char *prefix)
{
    if (str == NULL || prefix == NULL) return false;
    size_t prefix_len = strlen(prefix);
    return strncmp(str, prefix, prefix_len) == 0;
}

char *edc_strstrip(char *str)
{
    if (str == NULL) return NULL;

    char *end;

    while (isspace((unsigned char)*str))
        str++;

    if (*str == '\0')
        return str;

    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;

    *(end + 1) = '\0';
    return str;
}

size_t edc_strv_length(char **strv)
{
    size_t count = 0;
    if (strv == NULL) return 0;
    while (strv[count] != NULL)
        count++;
    return count;
}

void edc_strfreev(char **strv)
{
    if (strv == NULL) return;
    for (size_t i = 0; strv[i] != NULL; i++)
        free(strv[i]);
    free(strv);
}

char **edc_strsplit(const char *str, const char *delimiter, int max_tokens)
{
    if (str == NULL || delimiter == NULL) return NULL;

    size_t alloc = 16;
    size_t count = 0;
    char **result = malloc(alloc * sizeof(char *));
    if (result == NULL) return NULL;

    const char *start = str;
    size_t delim_len = strlen(delimiter);

    for (;;)
    {
        bool is_last_token = (max_tokens > 0 && (int)count == max_tokens - 1);
        const char *end;

        if (is_last_token)
        {
            end = start + strlen(start);
        }
        else
        {
            end = strstr(start, delimiter);
            if (end == NULL)
                end = start + strlen(start);
        }

        size_t len = end - start;
        char *token = malloc(len + 1);
        if (token == NULL)
        {
            edc_strfreev(result);
            return NULL;
        }
        memcpy(token, start, len);
        token[len] = '\0';

        if (count + 2 > alloc)
        {
            alloc *= 2;
            char **new_result = realloc(result, alloc * sizeof(char *));
            if (new_result == NULL)
            {
                result[count] = token;
                edc_strfreev(result);
                free(token);
                return NULL;
            }
            result = new_result;
        }

        result[count++] = token;

        if (is_last_token)
            break;

        if (*end == '\0')
            break;

        start = end + delim_len;
    }

    result[count] = NULL;
    return result;
}
