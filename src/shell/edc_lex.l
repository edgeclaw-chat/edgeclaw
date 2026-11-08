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
    C_UNQUOTED, C_QUOTED, C_BADQ, C_WS, C_PIPE, C_SEQ, C_ANDAND, C_OROR, C_BG, C_REDIR, C_OPT, C_OPTARG,
    C_SLASH
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
    /* bash quoting (see edc_cmd_parse.c): '…' is RAW, "…" carries only \" and \\, and the
     * escape-carrying form is ANSI-C -- spelled $'…' the way bash spells it, or '''…'''
     * (the in-tree alias). """…""" is "…" with a body that may hold a bare ".
     *
     * The triple bodies follow PYTHON's rule, not "no quote of this kind may appear":
     * the delimiter is a RUN of three, so a run of one or two inside the body is
     * literal text (`"""(he said "hi")"""`). Spelled as `(""{0,2}[^"])*` rather than
     * `[^"]*` for exactly that reason -- `[^"]*` was the bug: the block then failed to
     * match as a whole, fell through to the ORDINARY quote rule (longest match picks
     * `"…"` for the first two quotes), and ONE `"""…"""` reached the
     * parser as STRING/TEXT/STRING. On a shell line that is `syntax error, unexpected
     * T_TEXT, expecting end of file`; on `/task new` it silently became three argv
     * entries instead of one description.
     *
     * The rule has to agree with edc_cmd_unquote, the layer that strips the delimiters:
     * scan_region() with opener=3 closes only on three consecutive quotes, so the lexer
     * must hand it a body that may hold one or two. A trailing run of four or more still
     * behaves as it does in Python: the first three close the block and the leftovers
     * start the next token. */
"$'"[^']*"'"                 { push_quoted(yytext, false); }
"'''"('{0,2}[^'])*"'''"       { push_quoted(yytext, false); }
\"\"\"(\"{0,2}[^\"])*\"\"\"   { push_quoted(yytext, true); }
\"([^"\\]|\\.)*\"            { push_quoted(yytext, true); }
'[^']*'                      { push_quoted(yytext, false); }
"$'"[^']*$                   { chunk_t *c = push(C_BADQ); c->text = sdsnewlen(yytext, (size_t)yyleng); }
"'''"('{0,2}[^'])*$          { chunk_t *c = push(C_BADQ); c->text = sdsnewlen(yytext, (size_t)yyleng); }
\"\"\"(\"{0,2}[^\"])*$       { chunk_t *c = push(C_BADQ); c->text = sdsnewlen(yytext, (size_t)yyleng); }
\"([^\"\\]|\\.)*$            { chunk_t *c = push(C_BADQ); c->text = sdsnewlen(yytext, (size_t)yyleng); }
'[^']*$                      { chunk_t *c = push(C_BADQ); c->text = sdsnewlen(yytext, (size_t)yyleng); }
"2>>"                       { chunk_t *c = push(C_REDIR); c->fd = 2; c->append = true; }
"2>"                        { chunk_t *c = push(C_REDIR); c->fd = 2; }
">>"                        { chunk_t *c = push(C_REDIR); c->fd = 1; c->append = true; }
">"                         { chunk_t *c = push(C_REDIR); c->fd = 1; }
"<"                         { chunk_t *c = push(C_REDIR); c->fd = 0; }
"||"                        { push(C_OROR); }
"|"                         { push(C_PIPE); }
";"                         { push(C_SEQ); }
"&&"                        { push(C_ANDAND); }
"&"[ \t]*$                  { push(C_BG); }
    /* The trailing space is TRAILING CONTEXT (`/`), not part of the match: it must survive as
     * a C_WS chunk, because that chunk is what ends the current word. Consuming it here is
     * how `-d t1` used to arrive as ONE word `-dt1`: the option chunk carries the literal
     * name "-d", SHELL_OFF folds it into the word being built (options are not syntax there),
     * and with the space already eaten nothing separated it from `t1`. In SHELL_ON the
     * behaviour is unchanged -- TK_OPT/TK_OPTARG, and C_WS still emits no token.
     *
     * The input always has "\n" appended (see edc_lex), so a trailing `[ \t\n]` also covers
     * end-of-line; the old `([ \t\n]|$)` needed the `$` alternative only because it consumed
     * the separator. Values stay separate words -- and that is the whole point of splitting
     * here: `/task new x -d 1` gives ["x","-d","1"] (the handler's getopt sees the option)
     * while `/task new 'x -d 1'` gives ["x -d 1"] (one word, the text). */
"--bg"/[ \t\n]              { chunk_t *c = push(C_OPT); c->text = sdsnew("--bg"); }
"--new-group"/[ \t\n]       { chunk_t *c = push(C_OPT); c->text = sdsnew("--new-group"); }
"--with"/[ \t\n]            { chunk_t *c = push(C_OPTARG); c->text = sdsnew("--with"); }
"--group"/[ \t\n]           { chunk_t *c = push(C_OPTARG); c->text = sdsnew("--group"); }
"-d"/[ \t\n]                { chunk_t *c = push(C_OPTARG); c->text = sdsnew("-d"); }
"-g"/[ \t\n]                { chunk_t *c = push(C_OPTARG); c->text = sdsnew("-g"); }
^"/"[A-Za-z_-]+             { chunk_t *c = push(C_SLASH); c->text = sdsnewlen(yytext + 1, (size_t)yyleng - 1); }
[ \t\n]+                    { push(C_WS); }
    /* `$` is EXCLUDED from the bare-text run so `$'...` can start its own chunk.
     * Without this, `+$'a b'` split as UNQUOTED`+$` + QUOTED`a b`: the sigil stayed
     * glued to a `$`, became a plain word, and the whole line fell back to a message --
     * one of the four quoting forms silently did not work behind `+`. It cannot change
     * any word's VALUE: a lone `$` becomes its own C_UNQUOTED chunk and the word
     * builder joins adjacent chunks of the same kind (`a$b` -> `a` / `$` / `b` ->
     * `a$b`). It only lets the ANSI-C rule match where the form actually starts. */
[^ \t\n|;&<>"'$]+           { push_unquoted(yytext, (size_t)yyleng); }
.                           { push_unquoted(yytext, 1); }
%%
static void emit(edc_lex_out *out, edc_tok_kind k, sds text, bool refs, int fd, bool append, size_t b, size_t e)
{
    edc_tok *t = &out->toks[out->count++];
    t->kind = k;
    t->value = text;
    t->has_var_refs = refs;
    t->fd = fd;
    t->append = append;
    t->begin = b;
    t->end = e;
}

bool edc_lex(const char *src, edc_lex_mode mode, edc_lex_out *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = mode;
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
        /* `!` is a DISPATCH PREFIX, not shell syntax: the switch never gates it.
         * docs/input-syntax.zh.md says so in words ("`/` and `!` are not shell
         * syntax, the switch has no effect on them"), and `/` next door already
         * behaves that way. Under SHELL_OFF this used to fall through to one TK_TEXT,
         * so turning the shell syntax off silently took `!cmd` (and `!!`) away too --
         * a different promise than the one the manual and the UI badge make. */
        emit(out, TK_SIGIL_LOCALSHELL, NULL, false, 0, false, 0, 1);
        if (e > (size_t)(p - src))
            emit(out, TK_RAW_REST, sdsnewlen(p, e - (size_t)(p - src)), false, 0, false, (size_t)(p - src), e);
        emit(out, TK_EOF, NULL, false, 0, false, strlen(src), strlen(src));
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

    /* ── chunk → token：一个词 = 一个 TEXT 或一个 STRING ─────────────────────
     *
     * 引号区与裸文本**不混拼**（用户 2026-10-22 拍板）：`a"b c"d` 是三个词
     * `a` / `b c` / `d`，不是一个 `ab cd`。词的类型就是 token 的类型，所以
     * 「这个词是不是引号串」由 token 种类回答，不再需要 flag。
     *
     * 唯一的例外是被折叠的算符/选项（`disabled`）：SHELL_OFF 下 `|` `;` `&`
     * `-d` 都是普通文本，它们必须**粘进**同一个 TEXT —— `/session memo search a|b`
     * 仍然是一个词 `a|b`。它们不是"片段"，只是夹在片段之间的字节。 */
    sds cur = NULL;
    size_t wb = 0, we = 0;
    /* NONE / TEXT / STRING：当前正在攒的那个词是什么类型（和下面的 flush 共用） */
    enum { W_NONE = 0, W_TEXT, W_STRING } word = W_NONE;
    bool refs = false;

    for (int i = 0; i < g_nchunks; i++)
    {
        chunk_t *c = &g_chunks[i];
        const bool wordish = (c->kind == C_UNQUOTED || c->kind == C_QUOTED);
        /* 开关关闭：sigil/算符/选项**不成为 token**，降级成文本（并记一笔供 note 用）。
         * 两个例外，都不是“开关能管的语法”：
         *   C_BADQ  未闭合引号是词法错误 —— 降级成文本就是静默丢参；
         *   C_SLASH 行首 `/` 是派发前缀（命令层），与 shell 开关无关 ——
         *           它得成 TK_SLASH_CMD，否则命令行进不了文法。 */
        const bool disabled = (mode == EDC_LEX_SHELL_OFF) && !wordish && c->kind != C_WS &&
                              c->kind != C_BADQ && c->kind != C_SLASH;

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
                emit(out, TK_SIGIL_NEWGROUP, NULL, false, 0, false, 0, 1);
                sdsrange(c->text, 1, -1);
                if (sdslen(c->text) == 0)
                    continue;
            }
            else if (!cur && c->kind == C_UNQUOTED && c->b == 0 && c->text && c->text[0] == '+' &&
                     mode == EDC_LEX_SHELL_OFF)
            {
                out->saw_disabled = true;
            }
            /* 一个真正的 word 片段。类型一变就先把这个词吐出去（不混拼）；两个引号区
             * 相邻同理 —— 它们是两个词，不是 C 那样的字面量拼接。 */
            if (c->kind == C_QUOTED || c->kind == C_UNQUOTED)
            {
                const int want = (c->kind == C_QUOTED) ? W_STRING : W_TEXT;
                if (word != W_NONE && word != want)
                {
                    emit(out, word == W_STRING ? TK_STRING : TK_TEXT, cur, refs, 0, false, wb, we);
                    cur = NULL;
                    word = W_NONE;
                    refs = false;
                }
                word = want;
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
            if (c->kind == C_QUOTED && c->interp && c->text && strstr(c->text, "{{"))
                refs = true;
            continue;
        }

        /* 非 word：先把挂着的 word 吐出去 */
        if (cur)
        {
            emit(out, word == W_STRING ? TK_STRING : TK_TEXT, cur, refs, 0, false, wb, we);
            cur = NULL;
            word = W_NONE;
            refs = false;
        }

        switch (c->kind)
        {
        case C_WS: break;
        case C_PIPE: emit(out, TK_PIPE, NULL, false, 0, false, c->b, c->e); break;
        case C_SEQ: emit(out, TK_SEQ, NULL, false, 0, false, c->b, c->e); break;
        case C_ANDAND: emit(out, TK_ANDAND, NULL, false, 0, false, c->b, c->e); break;
        case C_OROR: emit(out, TK_OROR, NULL, false, 0, false, c->b, c->e); break;
        case C_BG: emit(out, TK_BG, NULL, false, 0, false, c->b, c->e); break;
        case C_REDIR: emit(out, TK_REDIR, NULL, false, c->fd, c->append, c->b, c->e); break;
        case C_OPT: emit(out, TK_OPT, sdsdup(c->text), false, 0, false, c->b, c->e); break;
        case C_OPTARG: emit(out, TK_OPTARG, sdsdup(c->text), false, 0, false, c->b, c->e); break;
        case C_BADQ:
            emit(out, TK_BAD_QUOTED, sdsdup(c->text), false, 0, false, c->b, c->e);
            break;
        case C_SLASH: emit(out, TK_SLASH_CMD, sdsdup(c->text), false, 0, false, c->b, c->e); break;
        default: break;
        }
    }
    if (cur)
        emit(out, word == W_STRING ? TK_STRING : TK_TEXT, cur, refs, 0, false, wb, we);

    emit(out, TK_EOF, NULL, false, 0, false, g_pos, g_pos);

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
