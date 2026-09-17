#include "edc_string_utils.h"

#include "utf8proc.h"
#include <mbedtls/base64.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *edc_strdup_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (len < 0)
        return NULL;

    char *buf = malloc((size_t)len + 1);
    if (!buf)
        return NULL;

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    va_end(ap);

    return buf;
}

int edc_strcmp0(const char *str1, const char *str2)
{
    if (!str1 && !str2)
        return 0;
    if (!str1)
        return -1;
    if (!str2)
        return 1;
    return strcmp(str1, str2);
}

int edc_str_equal(const void *v1, const void *v2)
{
    const char *s1 = (const char *)v1;
    const char *s2 = (const char *)v2;
    if (!s1 && !s2)
        return 1;
    if (!s1 || !s2)
        return 0;
    return (strcmp(s1, s2) == 0) ? 1 : 0;
}

int edc_direct_equal(const void *v1, const void *v2)
{
    return (v1 == v2) ? 1 : 0;
}

unsigned int edc_str_hash(const void *v)
{
    const unsigned char *p = (const unsigned char *)v;
    unsigned int h = 5381;
    if (!p)
        return h;
    int c;
    while ((c = *p++))
        h = ((h << 5) + h) + (unsigned int)c;
    return h;
}

unsigned int edc_direct_hash(const void *v)
{
    return (unsigned int)((unsigned long)v >> 2);
}

int edc_str_has_prefix(const char *str, const char *prefix)
{
    if (!str || !prefix)
        return 0;
    size_t prefix_len = strlen(prefix);
    if (prefix_len == 0)
        return 1;
    if (strlen(str) < prefix_len)
        return 0;
    return (strncmp(str, prefix, prefix_len) == 0) ? 1 : 0;
}

char *edc_strstrip(char *str)
{
    if (!str)
        return NULL;
    if (*str == '\0')
        return str;
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' || str[len - 1] == '\n' || str[len - 1] == '\r'))
        str[--len] = '\0';
    while (*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r'))
        str++;
    return str;
}

int edc_strv_length(char **str_array)
{
    if (!str_array)
        return 0;
    int n = 0;
    while (*str_array++)
        n++;
    return n;
}

char **edc_strsplit(const char *str, const char *delimiter, int max_tokens)
{
    if (!str || !delimiter)
        return NULL;

    size_t delim_len = strlen(delimiter);
    if (delim_len == 0)
        return NULL;

    char *buf = strdup(str);
    if (!buf)
        return NULL;

    char **result = calloc(64, sizeof(char *));
    if (!result)
    {
        free(buf);
        return NULL;
    }
    size_t capacity = 64;
    size_t count = 0;

    char *tok = buf;
    char *end;

    while ((end = strstr(tok, delimiter)) != NULL)
    {
        if (max_tokens > 0 && (int)count >= max_tokens - 1)
            break;

        if (count + 1 >= capacity)
        {
            capacity *= 2;
            result = realloc(result, capacity * sizeof(char *));
            if (!result)
            {
                edc_strfreev(result);
                free(buf);
                return NULL;
            }
        }

        *end = '\0';
        result[count++] = strdup(tok);
        tok = end + delim_len;
    }

    if (count + 1 >= capacity)
    {
        capacity = count + 2;
        result = realloc(result, capacity * sizeof(char *));
        if (!result)
        {
            edc_strfreev(result);
            free(buf);
            return NULL;
        }
    }

    result[count++] = strdup(tok);
    result[count] = NULL;

    free(buf);
    return result;
}

void edc_strfreev(char **str_array)
{
    if (!str_array)
        return;
    for (char **p = str_array; *p; p++)
        free(*p);
    free(str_array);
}

sds edc_str_utf8_truncate(const char *str, size_t max_chars, bool append_dots)
{
    if (!str)
        return sdsempty();

    size_t byte_count = 0;
    size_t char_count = 0;
    const unsigned char *p = (const unsigned char *)str;

    while (*p && char_count < max_chars)
    {
        utf8proc_int32_t uc = -1;
        ssize_t clen = utf8proc_iterate(p, -1, &uc);
        if (clen <= 0 || uc == -1)
        {
            /* invalid byte — still count as one "char" to avoid infinite loop */
            byte_count++;
            char_count++;
            p++;
        }
        else
        {
            byte_count += (size_t)clen;
            char_count++;
            p += clen;
        }
    }

    sds result = sdsnewlen(str, byte_count);
    if (append_dots && str[byte_count] != '\0')
    {
        result = sdscat(result, "...");
    }
    return result;
}

sds edc_str_utf8_make_valid(const char *str, ssize_t len)
{
    if (!str)
        return sdsempty();
    size_t n = (len < 0) ? strlen(str) : (size_t)len;

    sds result = sdsempty();
    result = sdsMakeRoomFor(result, n + n / 2);

    const unsigned char *p = (const unsigned char *)str;
    ssize_t remaining = (ssize_t)n;

    while (remaining > 0)
    {
        utf8proc_int32_t uc = -1;
        ssize_t clen = utf8proc_iterate(p, remaining, &uc);
        if (clen <= 0 || uc == -1)
        {
            result = sdscatlen(result, "\xEF\xBF\xBD", 3);
            p++;
            remaining--;
        }
        else
        {
            result = sdscatlen(result, (const char *)p, (size_t)clen);
            p += clen;
            remaining -= clen;
        }
    }

    return result;
}

sds edc_str_uri_escape(const char *str, size_t len)
{
    if (!str)
        return sdsempty();

    sds result = sdsempty();
    result = sdsMakeRoomFor(result, len * 3 + 1);

    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
        {
            result = sdscatlen(result, (const char *)&c, 1);
        }
        else
        {
            result = sdscatprintf(result, "%%%02X", c);
        }
    }

    return result;
}

sds edc_str_joinv(const char *separator, char **str_array)
{
    sds result = sdsempty();
    if (!str_array)
        return result;

    for (size_t i = 0; str_array[i]; i++)
    {
        if (i > 0 && separator)
            result = sdscat(result, separator);
        result = sdscat(result, str_array[i]);
    }

    return result;
}

sds edc_str_vprintf(const char *fmt, va_list ap)
{
    sds result = sdsempty();
    result = sdscatvprintf(result, fmt, ap);
    return result;
}

sds edc_base64_encode(const unsigned char *data, size_t len)
{
    size_t b64_len = 0;
    int ret = mbedtls_base64_encode(NULL, 0, &b64_len, data, len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
        return sdsempty();

    sds result = sdsempty();
    result = sdsMakeRoomFor(result, b64_len + 1);
    ret = mbedtls_base64_encode((unsigned char *)result, b64_len + 1, &b64_len, data, len);
    if (ret != 0)
    {
        sdsfree(result);
        return sdsempty();
    }
    sdsIncrLen(result, b64_len);
    return result;
}

sds edc_base64_decode(const char *b64_str, size_t *out_len)
{
    size_t b64_len = strlen(b64_str);
    size_t data_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &data_len, (const unsigned char *)b64_str, b64_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
    {
        if (out_len)
            *out_len = 0;
        return sdsempty();
    }

    sds result = sdsempty();
    result = sdsMakeRoomFor(result, data_len + 1);
    ret = mbedtls_base64_decode((unsigned char *)result, data_len + 1, &data_len, (const unsigned char *)b64_str,
                                b64_len);
    if (ret != 0)
    {
        sdsfree(result);
        if (out_len)
            *out_len = 0;
        return sdsempty();
    }

    ((unsigned char *)result)[data_len] = '\0';
    sdsIncrLen(result, data_len);
    if (out_len)
        *out_len = data_len;
    return result;
}
