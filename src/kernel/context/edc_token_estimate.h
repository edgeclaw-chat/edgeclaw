#ifndef __BC_TOKEN_ESTIMATE_H__
#define __BC_TOKEN_ESTIMATE_H__

#include <cJSON.h>
#include <stddef.h>

/* 启发式 token 估算 (见 .c 里的口径与边界)。只用于"provider 真值之后的尾部"
 * 与"没有真值时的整段估算"; 任何真值 (应答的 usage) 都优先于它。 */

/* 一段 UTF-8 文本的估算 (不含消息结构开销)。 */
extern long long edc_token_estimate_text(const char *s);

/* 一条消息 (cJSON 对象) 的估算: 其所有字符串值 + 每条消息的结构开销。 */
extern long long edc_token_estimate_msg(const cJSON *msg);

/* JSON 消息数组的 [from, to) 切片估算 (越界自动夹紧)。 */
extern long long edc_token_estimate_msgs(const cJSON *arr, size_t from, size_t to);

#endif
