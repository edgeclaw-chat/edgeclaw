#include "edc_mention.h"

#include "edc_cmd_parse.h"

#include <stdlib.h> /* strdup */
#include <string.h>

bool edc_mention_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* The ASCII punctuation an UNQUOTED reference hands back to the sentence: `看下 @a.c.`
 * names `a.c`. Emptying this set is a valid choice, but it must be made in one place --
 * hence the export (edc_mention_is_trailing_punct). */
bool edc_mention_is_trailing_punct(char c)
{
    return c != '\0' && strchr(",.):;!?]}", c) != NULL;
}

#define trailing_punct edc_mention_is_trailing_punct

/* A `#start[-end]` range right after |p| (which must be the '#'). Returns the length of
 * the range, or 0. It has to END the word -- otherwise `@f#1x` would swallow `#1`. */
static size_t range_suffix(const char *p)
{
    if (*p != '#')
        return 0;
    const char *q = p + 1;
    const char *first = q;
    while (*q >= '0' && *q <= '9')
        q++;
    if (q == first)
        return 0; /* `#` alone */
    if (*q == '-')
    {
        q++;
        const char *second = q;
        while (*q >= '0' && *q <= '9')
            q++;
        if (q == second)
            return 0; /* `#1-` */
    }
    if (*q != '\0' && !edc_mention_is_space(*q) && !trailing_punct(*q))
        return 0;
    return (size_t)(q - p);
}

/* `digits` or `digits-digits` filling |tail| entirely? That is the `#range` shape the pass
 * understands (and the one the display strips). */
static bool is_range_tail(const char *tail)
{
    const char *q = tail;
    const char *first = q;
    while (*q >= '0' && *q <= '9')
        q++;
    if (q == first)
        return false;
    if (*q == '-')
    {
        q++;
        const char *second = q;
        while (*q >= '0' && *q <= '9')
            q++;
        if (q == second)
            return false;
    }
    return *q == '\0';
}

char *edc_mention_name(const char *text, size_t at)
{
    char *token = NULL;
    const size_t len = edc_mention_parse(text, at, &token, NULL);
    if (len == 0 || !token)
    {
        sdsfree(token);
        return NULL;
    }
    char *hash = strrchr(token, '#');
    if (hash && is_range_tail(hash + 1))
        *hash = '\0'; /* the range is not part of the name */
    char *out = strdup(token);
    sdsfree(token);
    return out;
}

size_t edc_mention_parse(const char *text, size_t at, char **out_token, bool *out_self_closed)
{
    if (out_token)
        *out_token = NULL;
    if (out_self_closed)
        *out_self_closed = false;
    if (!text || text[at] != '@')
        return 0;

    const char *body = text + at + 1;
    if (*body == '\0')
        return 0; /* a bare `@` names nothing */

    char *token = NULL; /* an sds handle; the public type is char* (see the header) */
    const char *end = NULL;

    /* A quoted body: the quote rules are the lexer's (`edc_cmd_unquote` is what
     * push_quoted() calls too), so `@"…"`, `@'…'`, `@$'…'`, `@'''…'''` and `@"""…"""`
     * all mean here exactly what they mean on a shell line. */
    if (*body == '"' || *body == '\'' || (*body == '$' && body[1] == '\''))
    {
        const char *quoted_end = NULL;
        sds value = edc_cmd_unquote(body, &quoted_end);
        if (!value)
            return 0; /* unterminated: not a reference at all, so the text is left alone */
        token = value;
        end = quoted_end;

        /* A range may follow the closing quote (`@"my file.txt"#2-3`), which keeps the two
         * features composable instead of making the user choose. */
        const size_t rl = range_suffix(end);
        if (rl > 0)
        {
            token = sdscatlen(token, end, rl);
            end += rl;
        }
        if (out_self_closed)
            *out_self_closed = true;
    }
    else
    {
        const char *stop = body;
        while (*stop && !edc_mention_is_space(*stop))
            stop++;
        const char *trimmed = stop;
        while (trimmed > body && trailing_punct(trimmed[-1]))
            trimmed--;
        if (trimmed == body)
            return 0; /* only punctuation after the '@' */
        token = sdsnewlen(body, (size_t)(trimmed - body));
        /* The reference ENDS before the punctuation it gives back -- that is the span the
         * display highlights, and the pass simply keeps scanning the punctuation as
         * ordinary text (it must reach the model unchanged either way). */
        end = trimmed;
    }

    const size_t len = (size_t)(end - text) - at;
    if (out_token)
        *out_token = token; /* ownership moves to the caller */
    else
        sdsfree(token);
    return len;
}
