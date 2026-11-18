#include "edc_chat_bubble_line.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Append |s| to a growable buffer, escaping the four characters the format
 * reserves. Returns false on OOM. */
static bool append_escaped(char **buf, size_t *len, size_t *cap, const char *s)
{
    if (!s)
        s = "";
    const size_t need = strlen(s) * 2 + 1;
    if (*len + need > *cap)
    {
        size_t next = *cap ? *cap : 128;
        while (*len + need > next)
            next *= 2;
        char *grown = (char *)realloc(*buf, next);
        if (!grown)
            return false;
        *buf = grown;
        *cap = next;
    }
    for (const char *p = s; *p; p++)
    {
        switch (*p)
        {
        case '\\': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = '\\'; break;
        case '\n': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'n'; break;
        case '\r': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'r'; break;
        case '\t': (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 't'; break;
        default: (*buf)[(*len)++] = *p; break;
        }
    }
    return true;
}

static bool append_raw(char **buf, size_t *len, size_t *cap, const char *s)
{
    const size_t n = strlen(s);
    if (*len + n + 1 > *cap)
    {
        size_t next = *cap ? *cap : 128;
        while (*len + n + 1 > next)
            next *= 2;
        char *grown = (char *)realloc(*buf, next);
        if (!grown)
            return false;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    return true;
}

char *edc_chat_bubble_encode(const char *kind, int render_pref, const char *task_id,
                             const char *header, const char *content)
{
    char pref[32];
    snprintf(pref, sizeof(pref), "%d", render_pref);

    char *buf = NULL;
    size_t len = 0, cap = 0;
    const bool ok = append_escaped(&buf, &len, &cap, kind) && append_raw(&buf, &len, &cap, "\t") &&
                    append_raw(&buf, &len, &cap, pref) && append_raw(&buf, &len, &cap, "\t") &&
                    append_escaped(&buf, &len, &cap, task_id) && append_raw(&buf, &len, &cap, "\t") &&
                    append_escaped(&buf, &len, &cap, header) && append_raw(&buf, &len, &cap, "\t") &&
                    append_escaped(&buf, &len, &cap, content);
    if (!ok)
    {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}
