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
                             const char *header, const char *content, const char *highlight_lang)
{
    char pref[32];
    snprintf(pref, sizeof(pref), "%d", render_pref);

    char *buf = NULL;
    size_t len = 0, cap = 0;
    const bool ok = append_escaped(&buf, &len, &cap, kind) && append_raw(&buf, &len, &cap, "\t") &&
                    append_raw(&buf, &len, &cap, pref) && append_raw(&buf, &len, &cap, "\t") &&
                    append_escaped(&buf, &len, &cap, task_id) && append_raw(&buf, &len, &cap, "\t") &&
                    append_escaped(&buf, &len, &cap, header) && append_raw(&buf, &len, &cap, "\t") &&
                    append_escaped(&buf, &len, &cap, content) && append_raw(&buf, &len, &cap, "\t") &&
                    append_escaped(&buf, &len, &cap, highlight_lang);
    if (!ok)
    {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

/* Reverse of append_escaped: append |src| to |dst| turning the two-byte
 * sequences \\n \\r \\t \\\\ back into single bytes. */
static bool append_unescaped(char **buf, size_t *len, size_t *cap, const char *src)
{
    /* Always allocate at least one byte so the trailing NUL is safe even for
     * an empty field. */
    if (!*buf)
    {
        *buf = (char *)malloc(1);
        if (!*buf)
            return false;
        *cap = 1;
    }
    for (const char *p = src; *p; p++)
    {
        char c = *p;
        if (c == '\\' && p[1])
        {
            switch (p[1])
            {
            case 'n': c = '\n'; p++; break;
            case 'r': c = '\r'; p++; break;
            case 't': c = '\t'; p++; break;
            case '\\': c = '\\'; p++; break;
            default: break; /* stray backslash kept literally */
            }
        }
        if (*len + 2 > *cap)
        {
            size_t next = *cap ? *cap * 2 : 128;
            while (*len + 2 > next)
                next *= 2;
            char *grown = (char *)realloc(*buf, next);
            if (!grown)
                return false;
            *buf = grown;
            *cap = next;
        }
        (*buf)[(*len)++] = c;
    }
    (*buf)[*len] = '\0';
    return true;
}

static bool make_field(const char *src, size_t n, char **out)
{
    char *buf = (char *)malloc(n + 1);
    if (!buf)
        return false;
    memcpy(buf, src, n);
    buf[n] = '\0';
    /* Decode in place into a second buffer to keep it simple. */
    char *dec = NULL;
    size_t dlen = 0, dcap = 0;
    if (!append_unescaped(&dec, &dlen, &dcap, buf))
    {
        free(buf);
        free(dec);
        return false;
    }
    free(buf);
    *out = dec ? dec : (char *)calloc(1, 1);
    return *out != NULL;
}

bool edc_chat_bubble_decode_line(const char *line, char **kind, int *render_pref,
                                 char **task_id, char **header, char **content,
                                 char **highlight_lang)
{
    if (!line || !kind || !render_pref || !task_id || !header || !content || !highlight_lang)
        return false;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    /* Split on up to five raw tabs. Four tabs = legacy 5-field line (lang "");
     * five tabs = new 6-field line. Escaped tabs ("\\t") are not raw. */
    const char *start = line;
    const char *fields[6] = {0};
    size_t lens[6] = {0};
    int f = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (line[i] == '\t' && f < 5)
        {
            fields[f] = start;
            lens[f] = (size_t)(line + i - start);
            f++;
            start = line + i + 1;
        }
    }
    if (f != 4 && f != 5)
        return false;
    fields[f] = start;
    lens[f] = (size_t)(line + n - start);
    /* f==4 -> content is fields[4], lang empty; f==5 -> fields[4]=content, fields[5]=lang */
    const char *content_f = fields[4];
    size_t content_n = lens[4];
    const char *lang_f = (f == 5) ? fields[5] : "";
    size_t lang_n = (f == 5) ? lens[5] : 0;
    if (lens[0] == 0)
        return false;

    char *k = NULL, *t = NULL, *h = NULL, *c = NULL, *lang = NULL;
    bool ok = make_field(fields[0], lens[0], &k) &&
              make_field(fields[2], lens[2], &t) &&
              make_field(fields[3], lens[3], &h) &&
              make_field(content_f, content_n, &c) &&
              make_field(lang_f, lang_n, &lang);
    if (!ok)
    {
        free(k); free(t); free(h); free(c); free(lang);
        return false;
    }
    int pref = 0;
    for (size_t i = 0; i < lens[1]; i++)
    {
        char ch = fields[1][i];
        if (ch < '0' || ch > '9')
        { pref = 0; break; }
        pref = pref * 10 + (ch - '0');
    }
    *kind = k; *render_pref = pref; *task_id = t; *header = h;
    *content = c; *highlight_lang = lang;
    return true;
}
