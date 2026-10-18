#ifndef __EDC_LEX_H__
#define __EDC_LEX_H__
/* ── 新输入流水线的词法器（flex 生成） ──────────────────────────────────
 *
 * 与老的 edc_shell_token.c 并存：这一版**还没接进生产**（迁移第①步只落地+测试），
 * 老的 tokenizer 仍是唯一在用的那个。第③步再切换。
 *
 * 一个 word 就是一个 shell word：相邻片段（引号区 / 非引号区）**拼接**而成，
 * value 里引号已去掉、转义已处理 —— 切词从 edc_cmd_parse 搬到了这里。 */
#include "sds.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    TK_SLASH_CMD,         /* /name（位置 0） */
    TK_WORD,              /* 一个 shell word（fully_quoted = 恰好一个闭合引号区） */
    TK_BAD_QUOTED,        /* 引号开了却没闭合 —— token **自己就是**那个错误，不走侧信道 */
    TK_SIGIL_NEWGROUP,    /* + （位置 0） */
    TK_SIGIL_LOCALSHELL,  /* ! （位置 0，其载荷整体是下一个 TK_RAW_REST） */
    TK_RAW_REST,          /* ! 之后到输入尾的原文 */
    TK_PIPE,              /* | */
    TK_SEQ,               /* ; */
    TK_ANDAND,            /* && */
    TK_OROR,              /* || */
    TK_BG,                /* & ，且其后到输入尾只有空白 */
    TK_REDIR,             /* > >> 2> < （fd/append 见 token） */
    TK_OPT,               /* --bg --new-group（无参） */
    TK_OPTARG,            /* --with -d -g --group（后面吃一个 word 当参数） */
    TK_EOF,
} edc_tok_kind;

typedef enum
{
    EDC_LEX_SHELL_ON,
    EDC_LEX_SHELL_OFF,
} edc_lex_mode;

typedef struct
{
    edc_tok_kind kind;
    sds value;         /* TK_SLASH_CMD / TK_WORD / TK_RAW_REST / TK_OPT：文本（引号已去） */
    bool fully_quoted; /* TK_WORD：整个 word 恰由一个引号区构成（== 行首是引号） */
    bool has_var_refs; /* TK_WORD：含 {{…}}（只在双引号区里认） */
    int fd;            /* TK_REDIR：0=输入 / 1=stdout / 2=stderr */
    bool append;       /* TK_REDIR：>> 或 2>> */
    size_t begin, end; /* 在输入中的字节区间 */
} edc_tok;

typedef struct
{
    edc_tok *toks;
    int count;
    bool saw_disabled; /* 开关关闭时见过 sigil/算符（→ note，Ⓔ） */
    size_t disabled_at;/* 第一个越界 token 的位置 */
} edc_lex_out;

/* 词法器永不失败：不认识的字节也只是一段 word。返回 false 只在内存分配失败时。 */
bool edc_lex(const char *src, edc_lex_mode mode, edc_lex_out *out);
void edc_lex_out_free(edc_lex_out *out);

/* 一段文本 -> argv：切词**归词法器**（相邻片段拼接、引号已去），所以
 * `review "a b"` 得到 2 个参数而不是 3 个。命令层用它，算符不参与（SHELL_OFF）。 */
sds *edc_argv_from_tail(const char *tail, int *out_count);

#endif
