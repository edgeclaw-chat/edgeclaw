#include "edc_shell_token.h"
#include <stdlib.h>
#include <string.h>

void edc_shell_token_free(edc_shell_token_t *tok)
{
    if (!tok)
        return;
    if (tok->value)
        sdsfree(tok->value);
    tok->value = NULL;
}

edc_shell_token_t *edc_shell_tokenize(const char *input, int *count)
{
    int cap = 16;
    int n = 0;
    edc_shell_token_t *tokens = malloc((size_t)(cap + 1) * sizeof(edc_shell_token_t));
    sds cur = sdsempty();
    bool in_dq = false, in_sq = false;

#define EMIT_TEXT()                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        sdstrim(cur, " \t");                                                                                           \
        if (sdslen(cur) > 0)                                                                                           \
        {                                                                                                              \
            if (n >= cap)                                                                                              \
            {                                                                                                          \
                cap *= 2;                                                                                              \
                tokens = realloc(tokens, (size_t)(cap + 1) * sizeof(edc_shell_token_t));                               \
            }                                                                                                          \
            tokens[n].type = TOK_TEXT;                                                                                 \
            tokens[n].value = cur;                                                                                     \
            n++;                                                                                                       \
            cur = sdsempty();                                                                                          \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            sdsfree(cur);                                                                                              \
            cur = sdsempty();                                                                                          \
        }                                                                                                              \
    } while (0)

#define EMIT_TOK(t)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (n >= cap)                                                                                                  \
        {                                                                                                              \
            cap *= 2;                                                                                                  \
            tokens = realloc(tokens, (size_t)(cap + 1) * sizeof(edc_shell_token_t));                                   \
        }                                                                                                              \
        tokens[n].type = (t);                                                                                          \
        tokens[n].value = NULL;                                                                                        \
        n++;                                                                                                           \
    } while (0)

    for (const char *p = input; *p; p++)
    {
        if (in_dq)
        {
            if (*p == '"')
            {
                cur = sdscatlen(cur, p, 1);
                in_dq = false;
            }
            else if (*p == '\\' && *(p + 1))
            {
                p++;                        /* skip backslash */
                cur = sdscatlen(cur, p, 1); /* add escaped char */
            }
            else
            {
                cur = sdscatlen(cur, p, 1);
            }
        }
        else if (in_sq)
        {
            if (*p == '\'')
            {
                cur = sdscatlen(cur, p, 1);
                in_sq = false;
            }
            else
            {
                cur = sdscatlen(cur, p, 1);
            }
        }
        else
        {
            if (*p == '|')
            {
                EMIT_TEXT();
                EMIT_TOK(TOK_PIPE);
            }
            else if (*p == ';')
            {
                EMIT_TEXT();
                EMIT_TOK(TOK_SEQ);
            }
            else if (*p == '&')
            {
                const char *peek = p + 1;
                while (*peek == ' ' || *peek == '\t')
                    peek++;
                if (*peek == '\0')
                {
                    EMIT_TEXT();
                    EMIT_TOK(TOK_BG);
                    while (*(p + 1) == ' ' || *(p + 1) == '\t')
                        p++;
                }
                else
                {
                    cur = sdscatlen(cur, p, 1);
                }
            }
            else if (*p == '>')
            {
                EMIT_TEXT();
                if (*(p + 1) == '>')
                {
                    EMIT_TOK(TOK_REDIR_APPEND);
                    p++;
                }
                else
                {
                    EMIT_TOK(TOK_REDIR_OUT);
                }
            }
            else if (*p == '<')
            {
                EMIT_TEXT();
                EMIT_TOK(TOK_REDIR_IN);
            }
            else if (*p == '"')
            {
                in_dq = true;
                cur = sdscatlen(cur, p, 1);
            }
            else if (*p == '\'')
            {
                in_sq = true;
                cur = sdscatlen(cur, p, 1);
            }
            else
            {
                cur = sdscatlen(cur, p, 1);
            }
        }
    }

    EMIT_TEXT();
    /* EMIT_TEXT() always leaves `cur` pointing at a fresh sds (emitted above,
     * or a new empty one); this final one is never handed to a token, so
     * release it here. */
    sdsfree(cur);
    memset(&tokens[n], 0, sizeof(edc_shell_token_t));
    *count = n;
    return tokens;
}

sds edc_shell_tokens_join(edc_shell_token_t *tokens, int start, int end)
{
    sds result = sdsempty();
    for (int i = start; i < end; i++)
    {
        if (tokens[i].type == TOK_TEXT && tokens[i].value)
        {
            if (sdslen(result) > 0)
                result = sdscat(result, " ");
            result = sdscatsds(result, tokens[i].value);
        }
        else if (tokens[i].type == TOK_BG)
        {
            result = sdscat(result, " &");
        }
    }
    sdstrim(result, " \t");
    return result;
}

int *edc_shell_tokens_split_pipe(edc_shell_token_t *tokens, int count, int *n)
{
    int cap = 4;
    *n = 0;
    int *ranges = malloc((size_t)cap * 2 * sizeof(int));
    int seg_start = 0;

    for (int i = 0; i < count; i++)
    {
        if (tokens[i].type == TOK_PIPE)
        {
            if (*n * 2 >= cap)
            {
                cap *= 2;
                ranges = realloc(ranges, (size_t)cap * 2 * sizeof(int));
            }
            ranges[*n * 2] = seg_start;
            ranges[*n * 2 + 1] = i;
            (*n)++;
            seg_start = i + 1;
        }
    }
    if (*n * 2 >= cap)
    {
        cap *= 2;
        ranges = realloc(ranges, (size_t)cap * 2 * sizeof(int));
    }
    ranges[*n * 2] = seg_start;
    ranges[*n * 2 + 1] = count;
    (*n)++;

    return ranges;
}

int *edc_shell_tokens_split_seq(edc_shell_token_t *tokens, int count, int *n)
{
    int cap = 4;
    *n = 0;
    int *ranges = malloc((size_t)cap * 2 * sizeof(int));
    int seg_start = 0;

    for (int i = 0; i < count; i++)
    {
        if (tokens[i].type == TOK_SEQ)
        {
            if (*n * 2 >= cap)
            {
                cap *= 2;
                ranges = realloc(ranges, (size_t)cap * 2 * sizeof(int));
            }
            ranges[*n * 2] = seg_start;
            ranges[*n * 2 + 1] = i;
            (*n)++;
            seg_start = i + 1;
        }
    }
    if (*n * 2 >= cap)
    {
        cap *= 2;
        ranges = realloc(ranges, (size_t)cap * 2 * sizeof(int));
    }
    ranges[*n * 2] = seg_start;
    ranges[*n * 2 + 1] = count;
    (*n)++;

    return ranges;
}
