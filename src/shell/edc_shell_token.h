#ifndef __EDC_SHELL_TOKEN_H__
#define __EDC_SHELL_TOKEN_H__

#include "sds.h"
#include <stdbool.h>

typedef enum
{
    TOK_TEXT,         /* ordinary text (unquoted or quoted) */
    TOK_PIPE,         /* | */
    TOK_SEQ,          /* ; */
    TOK_BG,           /* & (trailing background) */
    TOK_REDIR_OUT,    /* > */
    TOK_REDIR_APPEND, /* >> */
    TOK_REDIR_ERR,    /* 2> */
    TOK_REDIR_IN,     /* < */
} edc_tok_type_t;

typedef struct
{
    edc_tok_type_t type;
    sds value; /* text for TOK_TEXT, NULL for operator tokens */
} edc_shell_token_t;

void edc_shell_token_free(edc_shell_token_t *tok);

/* Tokenize input with full quote-awareness.
 * Returns NULL-terminated array, *count set to token count.
 * Caller frees each token with edc_shell_token_free, then free array. */
edc_shell_token_t *edc_shell_tokenize(const char *input, int *count);

#endif
