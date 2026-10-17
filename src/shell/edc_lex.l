%option noyywrap nodefault 8bit prefix="edclx"
%{
/* 新输入流水线的词法器 —— flex 源。
 * 尚未接进生产（第①步只落地 + 单测）；老的 edc_shell_token.c 仍是在用的那个。 */
#include "edc_lex.h"
#include "edc_cmd_parse.h"
#include <stdlib.h>
#include <string.h>

typedef enum
{
    C_UNQUOTED, C_QUOTED, C_WS, C_PIPE, C_SEQ, C_BG, C_REDIR, C_OPT, C_OPTARG,
    C_SLASH, C_SIGIL_NG, C_SIGIL_LS, C_REST
} chunk_kind;

typedef struct
{
    chunk_kind kind;
    sds text;   /* word 片段的（已去引号）文本；对 C_OPT 是选项名 */
    sds raw;    /* 匹配到的原文（OFF 模式要能把它降级回文本） */
    bool interp;/* 双引号区（参与 {{…}} 识别） */
    int fd;
    bool append;
    size_t b, e;
} chunk_t;

static chunk_t *g_chunks;
static int g_nchunks, g_cap;
static size_t g_pos, g_begin;

#define YY_USER_ACTION                                                                                  \
    do                                                                                                         \
    {                                                                                                          \
        g_begin = g_pos;                                                                                       \
        g_pos += (size_t)yyleng;                                                                               \
    } while (0);

static chunk_t *push(chunk_kind k)
{
    if (g_nchunks == g_cap)
    {
        g_cap = g_cap ? g_cap * 2 : 32;
        g_chunks = realloc(g_chunks, sizeof(chunk_t) * (size_t)g_cap);
    }
    chunk_t *c = &g_chunks[g_nchunks++];
    memset(c, 0, sizeof(*c));
    c->kind = k;
    c->b = g_begin;
    c->e = g_pos;
    c->raw = sdsnewlen(yytext, (size_t)yyleng);
    return c;
}

static void push_quoted(const char *raw, bool interp)
{
    const char *end = NULL;
    sds v = edc_cmd_unquote(raw, &end);
    chunk_t *c = push(C_QUOTED);
    c->text = v ? v : sdsnew(raw); /* 永不失败：认不出就把原文当值 */
    c->interp = interp;
}

static void push_unquoted(const char *raw, size_t len)
{
    chunk_t *c = push(C_UNQUOTED);
    c->text = sdsnewlen(raw, len);
}
%}
%%
"\"\"\""[^"]*"\"\"\""       { push_quoted(yytext, true); }
"'''"[^']*"'''"             { push_quoted(yytext, false); }
\"([^"\\]|\\.)*\"           { push_quoted(yytext, true); }
'([^'\\]|\\.)*'             { push_quoted(yytext, false); }
"2>>"                       { chunk_t *c = push(C_REDIR); c->fd = 2; c->append = true; }
"2>"                        { chunk_t *c = push(C_REDIR); c->fd = 2; }
">>"                        { chunk_t *c = push(C_REDIR); c->fd = 1; c->append = true; }
">"                         { chunk_t *c = push(C_REDIR); c->fd = 1; }
"<"                         { chunk_t *c = push(C_REDIR); c->fd = 0; }
"|"                         { push(C_PIPE); }
";"                         { push(C_SEQ); }
"&"[ \t]*$                  { push(C_BG); }
"--bg"([ \t\n]|$)           { chunk_t *c = push(C_OPT); c->text = sdsnew("--bg"); }
"--new-group"([ \t\n]|$)    { chunk_t *c = push(C_OPT); c->text = sdsnew("--new-group"); }
"--with"([ \t\n]|$)         { chunk_t *c = push(C_OPTARG); c->text = sdsnew("--with"); }
"--group"([ \t\n]|$)        { chunk_t *c = push(C_OPTARG); c->text = sdsnew("--group"); }
"-d"[ \t]                   { chunk_t *c = push(C_OPTARG); c->text = sdsnew("-d"); }
"-g"[ \t]                   { chunk_t *c = push(C_OPTARG); c->text = sdsnew("-g"); }
^"/"[A-Za-z_-]+             { chunk_t *c = push(C_SLASH); c->text = sdsnewlen(yytext + 1, (size_t)yyleng - 1); }
[ \t\n]+                    { push(C_WS); }
[^ \t\n|;&<>"' ]+           { push_unquoted(yytext, (size_t)yyleng); }
.                           { push_unquoted(yytext, 1); }
%%
static void emit(edc_lex_out *out, edc_tok_kind k, sds text, bool fq, bool refs, int fd, bool append, size_t b,
                 size_t e)
{
    edc_tok *t = &out->toks[out->count++];
    t->kind = k;
    t->value = text;
    t->fully_quoted = fq;
    t->has_var_refs = refs;
    t->fd = fd;
    t->append = append;
    t->begin = b;
    t->end = e;
}

bool edc_lex(const char *src, edc_lex_mode mode, edc_lex_out *out)
{
    memset(out, 0, sizeof(*out));
    g_chunks = NULL;
    g_nchunks = g_cap = 0;
    g_pos = g_begin = 0;

    /* 位置 0 的 `!`：整行余下都是 sh 的载荷，不扫算符（docs §3 铁律③）。
     * 放在扫描前，因为 flex 的最长匹配会让 unquoted-run 吃掉单字符的 sigil。 */
    if (src && src[0] == '!')
    {
        out->toks = calloc(3, sizeof(edc_tok));
        if (!out->toks)
            return false;
        const char *p = src + 1;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t e = strlen(src);
        while (e > (size_t)(p - src) && (src[e - 1] == ' ' || src[e - 1] == '\t'))
            e--;
        if (mode == EDC_LEX_SHELL_OFF)
        {
            /* 开关关闭：`!` 不是 sigil —— 整行就是一段文本，并记一笔给 note */
            out->saw_disabled = true;
            out->disabled_at = 0;
            emit(out, TK_WORD, sdsnew(src), false, false, 0, false, 0, strlen(src));
        }
        else
        {
            emit(out, TK_SIGIL_LOCALSHELL, NULL, false, false, 0, false, 0, 1);
            if (e > (size_t)(p - src))
                emit(out, TK_RAW_REST, sdsnewlen(p, e - (size_t)(p - src)), false, false, 0, false,
                     (size_t)(p - src), e);
        }
        emit(out, TK_EOF, NULL, false, false, 0, false, strlen(src), strlen(src));
        return true;
    }

    /* 补一个行尾：`$`（`&` 与选项的行尾判定）需要它 */
    sds buf = sdscatprintf(sdsempty(), "%s\n", src ? src : "");
    YY_BUFFER_STATE b = edclx_scan_string(buf);
    BEGIN(INITIAL);
    while (edclxlex() != 0)
    {
    }
    edclx_delete_buffer(b);
    sdsfree(buf);

    out->toks = calloc((size_t)g_nchunks + 1, sizeof(edc_tok));
    if (!out->toks)
        return false;

    /* ── chunk → token：相邻的 UNQUOTED/QUOTED 拼成一个 word（真实 shell 的切词） ── */
    sds cur = NULL;
    size_t wb = 0, we = 0;
    int nchunks_in_word = 0, nquoted = 0;
    bool refs = false;

    for (int i = 0; i < g_nchunks; i++)
    {
        chunk_t *c = &g_chunks[i];
        const bool wordish = (c->kind == C_UNQUOTED || c->kind == C_QUOTED);
        /* 开关关闭：sigil/算符/选项**不成为 token**，降级成文本（并记一笔供 note 用） */
        const bool disabled = (mode == EDC_LEX_SHELL_OFF) && !wordish && c->kind != C_WS && c->kind != C_REST;

        if (wordish || disabled)
        {
            if (disabled)
            {
                if (!out->saw_disabled)
                {
                    out->saw_disabled = true;
                    out->disabled_at = c->b;
                }
            }
            /* 位置 0 的 `+`：从首片段里拆出来（同上，最长匹配会吃掉它） */
            if (!cur && c->kind == C_UNQUOTED && c->b == 0 && c->text && c->text[0] == '+' &&
                mode == EDC_LEX_SHELL_ON)
            {
                emit(out, TK_SIGIL_NEWGROUP, NULL, false, false, 0, false, 0, 1);
                sdsrange(c->text, 1, -1);
                if (sdslen(c->text) == 0)
                    continue;
            }
            else if (!cur && c->kind == C_UNQUOTED && c->b == 0 && c->text && c->text[0] == '+' &&
                     mode == EDC_LEX_SHELL_OFF)
            {
                out->saw_disabled = true;
            }
            if (!cur)
            {
                cur = sdsempty();
                wb = c->b;
            }
            if (c->text)
                cur = sdscatsds(cur, c->text);
            else if (c->raw)
                cur = sdscatsds(cur, c->raw); /* 降级：用原文 */
            we = c->e;
            nchunks_in_word++;
            if (c->kind == C_QUOTED)
            {
                nquoted++;
                if (c->interp && c->text && strstr(c->text, "{{"))
                    refs = true;
            }
            continue;
        }

        /* 非 word：先把挂着的 word 吐出去 */
        if (cur)
        {
            emit(out, TK_WORD, cur, nchunks_in_word == 1 && nquoted == 1, refs, 0, false, wb, we);
            cur = NULL;
            nchunks_in_word = nquoted = 0;
            refs = false;
        }

        switch (c->kind)
        {
        case C_WS: break;
        case C_PIPE: emit(out, TK_PIPE, NULL, false, false, 0, false, c->b, c->e); break;
        case C_SEQ: emit(out, TK_SEQ, NULL, false, false, 0, false, c->b, c->e); break;
        case C_BG: emit(out, TK_BG, NULL, false, false, 0, false, c->b, c->e); break;
        case C_REDIR: emit(out, TK_REDIR, NULL, false, false, c->fd, c->append, c->b, c->e); break;
        case C_OPT: emit(out, TK_OPT, sdsdup(c->text), false, false, 0, false, c->b, c->e); break;
        case C_OPTARG: emit(out, TK_OPTARG, sdsdup(c->text), false, false, 0, false, c->b, c->e); break;
        case C_SLASH: emit(out, TK_SLASH_CMD, sdsdup(c->text), false, false, 0, false, c->b, c->e); break;
        case C_SIGIL_NG: emit(out, TK_SIGIL_NEWGROUP, NULL, false, false, 0, false, c->b, c->e); break;
        case C_SIGIL_LS: emit(out, TK_SIGIL_LOCALSHELL, NULL, false, false, 0, false, c->b, c->e); break;
        case C_REST:
            if (mode == EDC_LEX_SHELL_OFF)
            {
                if (!out->saw_disabled)
                {
                    out->saw_disabled = true;
                    out->disabled_at = c->b;
                }
            }
            else
                emit(out, TK_RAW_REST, sdsdup(c->text), false, false, 0, false, c->b, c->e);
            break;
        default: break;
        }
    }
    if (cur)
        emit(out, TK_WORD, cur, nchunks_in_word == 1 && nquoted == 1, refs, 0, false, wb, we);

    emit(out, TK_EOF, NULL, false, false, 0, false, g_pos, g_pos);

    for (int i = 0; i < g_nchunks; i++)
    {
        sdsfree(g_chunks[i].text);
        sdsfree(g_chunks[i].raw);
    }
    free(g_chunks);
    g_chunks = NULL;
    g_nchunks = g_cap = 0;
    return true;
}

void edc_lex_out_free(edc_lex_out *out)
{
    if (!out || !out->toks)
        return;
    for (int i = 0; i < out->count; i++)
        if (out->toks[i].value)
            sdsfree(out->toks[i].value);
    free(out->toks);
    out->toks = NULL;
    out->count = 0;
}
