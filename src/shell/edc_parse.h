#ifndef __EDC_PARSE_H__
#define __EDC_PARSE_H__
/* 文法层：把词法器给的 token 流解析成 AST。
 *
 * 两条入口，都进这一份文法：
 *   - shell 行（`"desc" --bg`、`+…`、`a | b`）—— 调用方先 edc_classify 确认是 SHELL；
 *   - 命令 行（`/cmd arg…`）—— 调用方把开关关掉（SHELL_OFF）后喂进来，产出
 *     EDC_AST_COMMAND 节点：命令名 + argv。命令层因此不再自己切参数。
 *
 * LOCALSHELL（`!cmd`）与 MESSAGE 不进文法：它们没有语法，节点由派发层直接构造。 */
#include "edc_ast.h"
#include "edc_lex.h"

/* 成功返回 true 并填 |out|（调用方用 edc_ast_free 释放）；失败返回 false 并把
 * 人类可读的原因写进 |*err|（调用方 sdsfree）。 */
bool edc_parse(const edc_lex_out *toks, edc_ast *out, sds *err);

/* The ONE wording for "options are only read after a quoted description". It now belongs to
 * the SHELL line only: the grammar owns `-d`/`-g` there, so an unquoted stage followed by an
 * option is still an error. The command layer no longer uses it -- `/task new` reads its own
 * options out of argv, where quoting (not this rule) decides what is text. */
#define EDC_QUOTED_DESC_HINT                                                                                           \
    "a task description with options must be quoted: write `\"<desc>\" --bg`, or drop the options"

#endif
