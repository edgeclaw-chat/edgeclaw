#ifndef __BC_TURN_H__
#define __BC_TURN_H__
#include "edc_containers.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_turn
{
    char *task_id; /* 序列化时标记归属 */
    EdcQueue messages;
};

extern struct edc_turn *edc_turn_new();
extern void edc_turn_free(struct edc_turn *turn);

/* 通用: 把一条完整消息 (接管所有权) 追加进 turn; 会做 UTF-8 清洗并补 ts/render。 */
extern void edc_turn_add_message(struct edc_turn *turn, cJSON *msg);
extern void edc_turn_add_user_message(struct edc_turn *turn, const char *content);
extern void edc_turn_add_assistant_message(struct edc_turn *turn, cJSON *msg);
/* tool 消息 = LLM 语境里的工具结果; displayed/header/render 是它上屏时的显示
 * 元数据 (由工具自己 emit 声明), 供 chat 回放重现实时那个气泡 (header 如 shell
 * 命令原文)。displayed=false 表示这次工具调用没有上屏 (回放同样跳过, 与实时一致)。
 * header 允许为 NULL/空 (工具就是这样 emit 的); render 取 "plain"/"markdown"。 */
extern void edc_turn_add_tool_message(struct edc_turn *turn, const char *tool_call_id, const char *name,
                                      const char *content, bool displayed, const char *header, const char *render);
#endif
