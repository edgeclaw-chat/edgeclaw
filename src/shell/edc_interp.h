#ifndef __EDC_INTERP_H__
#define __EDC_INTERP_H__
/* 派发 + 解释器。
 *
 * 分类就是"提交点"：看**第一个 token** 决定这条输入是命令、本地 shell、消息还是 shell 行，
 * 命中即定、不回退。只有 SHELL 才进文法（edc_parse），所以文法里没有"任意 token"的产生式。 */
#include "edc_ast.h"
#include "edc_channel_actor.h"
#include "edc_lex.h"
#include "sds.h"
#include <stdbool.h>

struct edc_reply_to;
struct edc_scheduler;
struct edc_session_manager;

typedef enum
{
    EDC_IN_MESSAGE,    /* 一条任务，原文 */
    EDC_IN_COMMAND,    /* /…：交给命令层（本函数不处理） */
    EDC_IN_LOCALSHELL, /* !…：交给本地 shell（本函数不处理） */
    EDC_IN_SHELL,      /* 走文法 */
} edc_in_kind;

/* |*err| 非空 = 拒绝并回这句话（例如 `+!cmd`、`+` 空内容）。 */
edc_in_kind edc_classify(const edc_lex_out *toks, sds *err);

/* 解释一棵 SHELL AST：每个 stage 一个任务；同一 pipeline 内 JOIN（注入），pipeline 之间
 * SEQUENCE（只等）。链尾 `&` 让每个 stage 都不占前台。 */
bool edc_interp_shell(struct edc_session_manager *sm, struct edc_scheduler *sched, struct edc_reply_to to,
                      struct edc_channel_message *message, const edc_ast *ast);

#endif
