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

/* A bare word may not follow the closing quote of a description: that is where the
 * OPTION area starts (`-d <id>`, `-g <group>`, `> file`). The token is quoted back so
 * the message points at the word the user has to move inside the quotes or delete.
 * EDC_AFTER_DESC_HINT carries the wording; edc_parse.h owns it, because the doc and
 * the message must not drift apart. */
static void after_desc_err(const edc_tok *t)
{
    if (!t || g_err)
        return;
    g_err = sdscatprintf(sdsempty(), EDC_AFTER_DESC_HINT, t->value ? t->value : "");
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
/* 带参选项后面没有值：`"desc" -d` 断了。这不是"未知语法"，是**少了东西**，所以给一句
 * 能照着改的话，而不是 bison 的 "unexpected T_WORD / end of file"。
 *
 * 它在 `edc_parse()` 的 token 预检里跑，不是文法动作：`opts T_OPTARG` 之后 bison 会先把
 * 空产生式 `remember` 规约掉再去等 T_WORD，所以那条"缺值"规则永远轮不到（试过，动作不执行）。
 * 预检也正好和上面 TK_BAD_QUOTED 的拦截同一个位置。 */
static sds opt_needs_value(const edc_tok *o)
{
    if (!o || !o->value)
        return NULL;
    const char *arg = (strcmp(o->value, "-d") == 0 || strcmp(o->value, "--with") == 0) ? "<task id>" : "<group id>";
    return sdscatprintf(sdsempty(), "`%s` needs a value: write `\"desc\" %s %s`.", o->value, o->value, arg);
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
    /* `<` 是"把文件读进这条任务"，这件事归 `@file`（passes 里读，过沙箱与 workdir）。
     * 不静默丢弃：旧的写法必须停下来，并告诉用户换成什么。 */
    if (spec && spec->fd == 0)
    {
        set_err("input redirect `<` is gone — put the file in the message as `@file` "
                "(it is read the same way, through the sandbox)");
        return;
    }

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

/* `/cmd arg…`: the grammar owns the node, the command layer reads it. Two actions,
 * one per half of the rule, so the name comes from the slash token itself (g_cur
 * always points at the token that was just shifted). */
static void cmd_begin(const edc_tok *tok)
{
    g_ast->kind = EDC_AST_COMMAND;
    g_ast->cmd = calloc(1, sizeof(*g_ast->cmd));
    if (!g_ast->cmd)
        return;
    g_ast->cmd->name = sdsdup((tok && tok->value) ? tok->value : sdsempty());
}

static void cmd_add(const edc_tok *tok)
{
    if (!g_ast->cmd)
        return;
    struct edc_ast_cmd *c = g_ast->cmd;
    c->argv = realloc(c->argv, sizeof(sds) * (size_t)(c->argc + 1));
    c->argv_quoted = realloc(c->argv_quoted, sizeof(bool) * (size_t)(c->argc + 1));
    c->argv[c->argc] = sdsdup((tok && tok->value) ? tok->value : sdsempty());
    /* The AST outlives the token stream, so the one fact it needs to keep is which words
     * were quoted (TK_STRING) -- `/prompt review "--strict"` sends the text, a bare
     * `--strict` is option-shaped and refused. */
    c->argv_quoted[c->argc] = tok && tok->kind == TK_STRING;
    c->argc++;
}

%}
%token T_TEXT T_STRING T_NEWGROUP T_PIPE T_SEQ T_ANDAND T_OROR T_BG T_REDIR T_OPT T_OPTARG T_SLASH_CMD T_EOF2
/* 显式写出来：bison 默认把**第一条规则的左部**当起始符号，而 `word` 现在排在 `input`
 * 前面（它是两者的公共零件），不加这一行起始符号就成了 `word` —— 症状是每条输入都报
 * "unexpected T_SLASH_CMD, expecting T_TEXT or T_STRING"。 */
%start input
%%
/* 一个「词」：无引号文本，或一个引号串。选项取值、命令参数、重定向目标都用它 ——
 * 所以 `-d t1`、`-d "t 1"`、`-d 'D:\\my repo'` 三种写法都成立，而哪种是哪种由
 * token 种类说清楚，不用再挂 flag。 */
word
    : T_TEXT
    | T_STRING
    ;

input
    : stage_head rest
    | T_NEWGROUP stage_head rest        { g_ast->new_group = true; }
    | cmd_head cmd_args
    ;
/* `/cmd arg…`: a command line. Both operands are ordinary shell words (the caller
 * lexes a command line with the shell switch OFF, so `|`, `&`, `--bg` … are plain
 * arguments here, exactly as they always were for `/…`). */
cmd_head
    : T_SLASH_CMD                       { cmd_begin(g_cur); }
    ;
cmd_args
    : /* empty */
    | cmd_args word                     { cmd_add(g_cur); }
    ;
/* shell 行的一个 stage：描述**必须是引号串** —— 现在这是文法说的，不是动作里查的。
 * 第二条产生式只为那句话服务：裸文本进了 stage 位置就给可读的提示，而不是让 bison
 * 抛一句 unexpected。 */
stage_head
    : T_STRING                          { stage_begin(); stage_set_word(g_cur); } opts
    | T_TEXT                            { set_err(EDC_QUOTED_DESC_HINT); YYERROR; }
    ;
rest
    : /* empty */
    | T_BG                              { g_ast->background = true; }
    | T_PIPE stage_head rest
    | T_SEQ seq_pipe stage_head rest
    | T_ANDAND strict_pipe stage_head rest
    | T_OROR unless_pipe stage_head rest
    /* docs §"引号之后只能跟选项": a bare word after the closing quote is an ERROR
     * (`"hello" and more`), never silently dropped -- the old tokenizer accepted the
     * description and threw the rest away. bison's default message here is
     * "unexpected T_TEXT, expecting end of file", which names neither the rule nor the
     * fix, so these two productions exist only to say it in words (same trick as
     * stage_head's T_TEXT line above). */
    | T_TEXT                            { after_desc_err(g_cur); YYERROR; }
    | T_STRING                          { after_desc_err(g_cur); YYERROR; }
    ;
/* 中规则动作（独立非终结符）: `&&` 的 strict 位必须落在**它自己**新建的那个 pipeline 上。
 * 写在这条规则的末尾不行 —— rest 会继续建 pipeline, 那时 g_pipe 已经指向最后一个了。 */
seq_pipe
    : /* empty */                       { pipe_new(); }
    ;
strict_pipe
    : /* empty */                       { pipe_new(); g_pipe->chain = EDC_CHAIN_AND; }
    ;
unless_pipe
    : /* empty */                       { pipe_new(); g_pipe->chain = EDC_CHAIN_OR; }
    ;
opts
    : /* empty */
    | opts T_OPT                        { opt_noarg(g_cur); }
    | opts T_OPTARG remember word        { opt_witharg(g_remember, g_cur); }
    | opts T_REDIR remember word         { redir_add(g_cur, g_remember); }
    ;
remember
    : /* empty */                       { g_remember = g_cur; }
    ;
%%
bool edc_parse(const edc_lex_out *toks, edc_ast *out, sds *err)
{
    memset(out, 0, sizeof(*out));
    out->kind = EDC_AST_SHELL; /* 文法产出 SHELL 与 COMMAND；MESSAGE 由派发层直接构造 */
    if (err)
        *err = NULL;

    /* 未闭合的字面量在哪个位置都不合法：先在 token 层拦一次，文案比 bison 的默认好。
     * 带参选项缺值同理（`"desc" -d` / `"desc" --group`）：那也不是"未知语法"，是少了东西。 */
    for (int i = 0; toks && i < toks->count; i++)
    {
        if (toks->toks[i].kind == TK_BAD_QUOTED)
        {
            if (err)
                *err = edc_lex_unterminated_quote(toks->toks[i].value, toks->toks[i].begin + 1);
            return false;
        }
        if (toks->toks[i].kind == TK_OPTARG)
        {
            const edc_tok *next = (i + 1 < toks->count) ? &toks->toks[i + 1] : NULL;
            if (!next || (next->kind != TK_TEXT && next->kind != TK_STRING))
            {
                sds msg = opt_needs_value(&toks->toks[i]);
                if (err)
                    *err = msg;
                else
                    sdsfree(msg);
                return false;
            }
        }
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
    case TK_TEXT: return T_TEXT;
    case TK_STRING: return T_STRING;
    case TK_SLASH_CMD: return T_SLASH_CMD;
    case TK_SIGIL_NEWGROUP: return T_NEWGROUP;
    case TK_PIPE: return T_PIPE;
    case TK_SEQ: return T_SEQ;
    case TK_ANDAND: return T_ANDAND;
    case TK_OROR: return T_OROR;
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
    if (a->cmd)
    {
        sdsfree(a->cmd->name);
        for (int i = 0; i < a->cmd->argc; i++)
            sdsfree(a->cmd->argv[i]);
        free(a->cmd->argv);
        free(a->cmd->argv_quoted);
        free(a->cmd);
    }
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
