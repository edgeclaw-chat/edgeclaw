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
                /* Copy the pair verbatim: unescaping belongs to the argument parser
                 * (the only place that knows this text is a task description). Not
                 * flipping the quote state on the escaped char is what keeps `\"`
                 * from ending the region. */
                cur = sdscatlen(cur, p, 1);
                p++;
                cur = sdscatlen(cur, p, 1);
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
            else if (*p == '\\' && *(p + 1))
            {
                cur = sdscatlen(cur, p, 1);
                p++;
                cur = sdscatlen(cur, p, 1);
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
                /* `2>`: the `2` was already collected into the text. Recognise it HERE, at
                 * the operator, and take it back out. (The old place was the AST, checking
                 * whether the command text ends with a `2` -- which only worked while the
                 * description was unquoted. With every string quoted, the `2` lands in the
                 * option area and is gone by then.) */
                bool stderr_redir = false;
                size_t clen = sdslen(cur);
                if (clen >= 1 && cur[clen - 1] == '2' &&
                    (clen == 1 || cur[clen - 2] == ' ' || cur[clen - 2] == '\t'))
                {
                    stderr_redir = true;
                    sdssetlen(cur, clen - 1);
                    sdstrim(cur, " \t");
                }

                EMIT_TEXT();
                if (*(p + 1) == '>')
                {
                    EMIT_TOK(TOK_REDIR_APPEND);
                    p++;
                }
                else if (stderr_redir)
                {
                    EMIT_TOK(TOK_REDIR_ERR);
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

