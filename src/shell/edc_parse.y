%define api.value.type {int}
%define parse.error verbose
%{
/* shell 行的文法。种子是 docs/design/shell.y ——
 * 那份已被 bison 验过 0 冲突；这里加上动作与"选项/重定向"的收集。
 *
 * 动作里**只建 AST，绝不产生副作用**（§13.3）：bison 边解析边执行动作，一旦在动作里
 * 建任务，后面遇到语法错误就会留下半成品。
 *
 * 分类不在这里：调用方先用 edc_classify() 看第一个 token 决定 COMMAND / LOCALSHELL /
 * MESSAGE / SHELL，只有 SHELL 才进这个文法 —— 所以这里没有"任意 token"的产生式。 */
#include "edc_parse.h"

#include <stdlib.h>
#include <string.h>

/* 定义在 epilogue（那里才有 token 枚举），先声明。 */
int yylex(void);
void yyerror(const char *msg);

static const edc_lex_out *g_toks;
static int g_i;
static const edc_tok *g_cur;          /* 刚被 shift 的 token（动作里读它） */
static sds g_err;
static edc_ast *g_ast;
static struct edc_ast_pipeline *g_pipe; /* 最后一个 pipeline */
static struct edc_ast_stage *g_stage;   /* 正在建的 stage */
static const edc_tok *g_remember; /* 上一步 shift 的 token（选项名 / 重定向） */

static void set_err(const char *msg)
{
    if (!g_err && msg)
        g_err = sdsnew(msg);
}
static struct edc_ast_stage *cur_stage(void) { return g_stage; }

/* ── 建树 ─────────────────────────────────────────────────── */
static void pipe_new(void)
{
    struct edc_ast_pipeline *p = calloc(1, sizeof(*p));
    if (!g_pipe)
        g_ast->pipelines = p;
    else
        g_pipe->next = p;
    g_pipe = p;
}
static void stage_begin(void)
{
    struct edc_ast_stage *s = calloc(1, sizeof(*s));
    if (!g_pipe)
        pipe_new();
    struct edc_ast_stage **tail = &g_pipe->stages;
    while (*tail)
        tail = &(*tail)->next;
    *tail = s; /* 同一 pipeline 内：JOIN */
    g_stage = s;
}
static void stage_set_word(const edc_tok *w)
{
    g_stage->msg.text = sdsdup(w->value ? w->value : sdsempty());
    g_stage->has_var_refs = w->has_var_refs;
}
/* 无参选项：shell 行里只允许这两个以外的一个都没有 —— `--bg`/`--new-group` 属于命令层。 */
static void opt_noarg(const edc_tok *o)
{
    if (!o || !o->value)
        return;
    if (strcmp(o->value, "--bg") == 0)
        set_err("`--bg` belongs to the command layer; a shell line says `&` at the end");
    else if (strcmp(o->value, "--new-group") == 0)
        set_err("`--new-group` belongs to the command layer; a shell line says `+` at the start");
    else
        set_err("unknown option");
}
/* 带参选项：参数是紧接的那个 word（所以 `-d \"a b\"` 也成立）。 */
static void opt_witharg(const edc_tok *o, const edc_tok *val)
{
    struct edc_ast_stage *s = cur_stage();
    if (!o || !o->value || !val)
        return;
    if (strcmp(o->value, "-d") == 0 || strcmp(o->value, "--with") == 0)
    {
        s->deps = realloc(s->deps, sizeof(sds) * (size_t)(s->ndep + 1));
        s->deps[s->ndep++] = sdsdup(val->value ? val->value : sdsempty());
    }
    else if (strcmp(o->value, "-g") == 0 || strcmp(o->value, "--group") == 0)
    {
        if (s->group)
            sdsfree(s->group);
        s->group = sdsdup(val->value ? val->value : sdsempty());
    }
}
static void redir_add(const edc_tok *target, const edc_tok *spec)
{
    struct edc_ast_redir *r = calloc(1, sizeof(*r));
    r->fd = spec->fd;
    r->append = spec->append;
    r->target = sdsdup(target->value ? target->value : sdsempty());
    struct edc_ast_stage *s = cur_stage();
    struct edc_ast_redir **tail = &s->redirs;
    while (*tail)
        tail = &(*tail)->next;
    *tail = r; /* 同 fd 的 last wins 由解释器按顺序覆盖 */
}

%}
%token T_WORD T_NEWGROUP T_PIPE T_SEQ T_BG T_REDIR T_OPT T_OPTARG T_EOF2
%%
input
    : stage_head rest
    | T_NEWGROUP stage_head rest        { g_ast->new_group = true; }
    ;
stage_head
    : T_WORD                            { if (!g_cur->fully_quoted)
                                          {
                                              set_err("a shell stage must be one quoted string, e.g. "
                                                      "`\"text\"` -- an unquoted word there is ambiguous");
                                              YYERROR;
                                          }
                                          stage_begin(); stage_set_word(g_cur); } opts
    ;
rest
    : /* empty */
    | T_BG                              { g_ast->background = true; }
    | T_PIPE stage_head rest
    | T_SEQ pipe_new stage_head rest
    ;
pipe_new
    : /* empty */                       { pipe_new(); }
    ;
opts
    : /* empty */
    | opts T_OPT                        { opt_noarg(g_cur); }
    | opts T_OPTARG remember T_WORD     { opt_witharg(g_remember, g_cur); }
    | opts T_REDIR remember T_WORD      { redir_add(g_cur, g_remember); }
    ;
remember
    : /* empty */                       { g_remember = g_cur; }
    ;
%%
bool edc_parse(const edc_lex_out *toks, edc_ast *out, sds *err)
{
    memset(out, 0, sizeof(*out));
    out->kind = EDC_AST_SHELL; /* 文法只产出这一种；MESSAGE 节点由派发层直接构造 */
    if (err)
        *err = NULL;

    /* 未闭合的字面量在哪个位置都不合法：先在 token 层拦一次，文案比 bison 的默认好。 */
    for (int i = 0; toks && i < toks->count; i++)
        if (toks->toks[i].kind == TK_BAD_QUOTED)
        {
            if (err)
                *err = sdsnew("Unterminated quoted string — `\"…\"` on a shell line must close.");
            return false;
        }

    g_toks = toks;
    g_i = 0;
    g_cur = NULL;
    g_err = NULL;
    g_ast = out;
    g_pipe = NULL;
    g_stage = NULL;
    g_remember = NULL;

    const bool ok = (yyparse() == 0 && !g_err);
    if (!ok)
    {
        edc_ast_free(out);
        if (err)
            *err = g_err ? g_err : sdsnew("syntax error");
        else if (g_err)
            sdsfree(g_err);
        g_err = NULL;
        return false;
    }
    if (err)
        *err = NULL;
    if (g_err)
        sdsfree(g_err);
    g_err = NULL;
    return true;
}

/* ── 词法回喂 ─────────────────────────────────────────────── */
int yylex(void)
{
    if (!g_toks || g_i >= g_toks->count)
        return 0;
    g_cur = &g_toks->toks[g_i++];
    switch (g_cur->kind)
    {
    case TK_WORD: return T_WORD;
    case TK_SIGIL_NEWGROUP: return T_NEWGROUP;
    case TK_PIPE: return T_PIPE;
    case TK_SEQ: return T_SEQ;
    case TK_BG: return T_BG;
    case TK_REDIR: return T_REDIR;
    case TK_OPT: return T_OPT;
    case TK_OPTARG: return T_OPTARG;
    case TK_EOF: return 0; /* bison 的 EOF 是 token 0 */
    default: return T_EOF2; /* EOF / 不该出现在 shell 行里的（slash/sigil !/raw_rest） */
    }
}
void yyerror(const char *msg)
{
    if (!g_err)
        g_err = sdsnew(msg ? msg : "syntax error");
}

void edc_msg_payload_add_part(struct edc_msg_payload *payload, enum edc_msg_role role, bool synthetic,
                             const char *content)
{
    if (!payload || !content)
        return;
    payload->parts = realloc(payload->parts, sizeof(*payload->parts) * (size_t)(payload->nparts + 1));
    struct edc_msg_part *p = &payload->parts[payload->nparts++];
    p->role = role;
    p->synthetic = synthetic;
    p->content = sdsnew(content);
}

static void payload_free(struct edc_msg_payload *payload)
{
    if (!payload)
        return;
    if (payload->text)
        sdsfree(payload->text);
    for (int i = 0; i < payload->nparts; i++)
        sdsfree(payload->parts[i].content);
    free(payload->parts);
    payload->parts = NULL;
    payload->nparts = 0;
}

void edc_ast_free(edc_ast *a)
{
    if (!a)
        return;
    payload_free(&a->message);
    struct edc_ast_pipeline *p = a->pipelines;
    while (p)
    {
        struct edc_ast_pipeline *pn = p->next;
        struct edc_ast_stage *s = p->stages;
        while (s)
        {
            struct edc_ast_stage *sn = s->next;
            struct edc_ast_redir *r = s->redirs;
            while (r)
            {
                struct edc_ast_redir *rn = r->next;
                sdsfree(r->target);
                free(r);
                r = rn;
            }
            for (int i = 0; i < s->ndep; i++)
                sdsfree(s->deps[i]);
            free(s->deps);
            if (s->group)
                sdsfree(s->group);
            payload_free(&s->msg);
            free(s);
            s = sn;
        }
        free(p);
        p = pn;
    }
    memset(a, 0, sizeof(*a));
}
