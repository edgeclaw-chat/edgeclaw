#ifndef __EDC_PARSE_H__
#define __EDC_PARSE_H__
/* 文法层：把词法器给的 token 流（**已经由派发层确认是 shell 行**）解析成 AST。
 *
 * 不负责分类：调用方看第一个 token 决定 COMMAND / LOCALSHELL / MESSAGE / SHELL，
 * 只有 SHELL 才走到这里。 */
#include "edc_ast.h"
#include "edc_lex.h"

/* 成功返回 true 并填 |out|（调用方用 edc_ast_free 释放）；失败返回 false 并把
 * 人类可读的原因写进 |*err|（调用方 sdsfree）。 */
bool edc_parse(const edc_lex_out *toks, edc_ast *out, sds *err);

#endif
