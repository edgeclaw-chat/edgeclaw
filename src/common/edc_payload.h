#ifndef __EDC_PAYLOAD_H__
#define __EDC_PAYLOAD_H__
/* 一条用户输入的"旁挂载荷"：占位符 key -> 原文。
 *
 * 用途只有一个：**输入框折叠**。用户粘一大段文本时，缓冲区里只留一行占位符
 * （`[Pasted #1 · 42 lines]`），原文住在这里。于是：
 *
 *   text    = 用户在框里看到的（含占位符）
 *   定稿文本 = edc_payload_expand(text, payload) = 语义上的那行字
 *
 * 语义一律看定稿文本：分类（edc_commands_input_kind）、lex、依赖、徽标、落盘
 * 都用它。**折叠只是显示** —— 这一条是这条流水线的地基，见 edc_text_passes.h。
 *
 * 为什么键是"文本"而不是 id：占位符本身就是 key，展开就是一次纯文本替换，
 * 不需要坐标、不需要维护区间（编辑过的缓冲区里坐标会失效，key 不会）。
 *
 * 顺序：entries 保序（插入顺序 = 粘贴顺序）。展开按 key 长度从长到短匹配，
 * 避免一个 key 是另一个的前缀时吃错。
 *
 * 落盘形态：JSON 对象（key -> 文本），随 user message 一起存（见
 * edc_turn_add_user_message / edc_state_serializer）。读不到就是"没有载荷"，
 * 老数据天然兼容。 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Plain char* on the surface, sds inside (edc_payload.c): this header is also consumed
 * from C++ (the input widget), and sds.h does not compile as C++. The two are the same
 * pointer type, so nothing is lost -- only the freeing call has to go through us
 * (edc_payload_string_free) instead of sdsfree. */

struct edc_payload_entry
{
    char *key;  /* 占位符文本，原样出现在 text 里 */
    char *text; /* 展开成的正文 */
};

struct edc_payload
{
    struct edc_payload_entry *items;
    int n;
    int cap;
};

/* NULL-safe 的读写；set 覆盖同名 key（顺序不变），remove 不存在的 key 返回 false。 */
struct edc_payload *edc_payload_new(void);
void edc_payload_free(struct edc_payload *payload);
bool edc_payload_set(struct edc_payload *payload, const char *key, const char *text);
const char *edc_payload_get(const struct edc_payload *payload, const char *key);
bool edc_payload_remove(struct edc_payload *payload, const char *key);
int edc_payload_count(const struct edc_payload *payload);

/* 空载荷（NULL 或 0 条）= "没有东西要展开"。所有调用点都先问这一句，
 * 好让普通输入这条路上一次分配都不多花。 */
static inline bool edc_payload_empty(const struct edc_payload *payload)
{
    return !payload || payload->n == 0;
}

/* JSON 互转。to_json 在空载荷时返回 NULL（不写 "{}"，省得每条消息多一个字段）；
 * from_json 对 NULL / 空串 / 坏 JSON 一律返回 NULL，不报错 —— 载荷永远是可用可无的。 */
char *edc_payload_to_json(const struct edc_payload *payload);
struct edc_payload *edc_payload_from_json(const char *json);

/* 定稿：把 |text| 里出现的每个 key 换成它的正文。**单遍、不递归**（换进去的正文
 * 不再被扫描，所以粘贴内容里的 `@file` / `{{…}}` / 另一个占位符都不会被二次展开）、
 * **幂等**（第一次之后 key 已不在文本里，再调一次是原样拷贝；唯一例外是正文里自己
 * 含有同名字符串的自指载荷）。
 *
 * 返回新串（edc_payload_string_free 释放），永不为 NULL；|text| 为 NULL 时返回空串。 */
char *edc_payload_expand(const char *text, const struct edc_payload *payload);

/* 文本里**找不到占位符**的那些 entry（用户把 chip 改坏/删掉了）：按 entries 顺序写进
 * |out|（最多 |cap| 条，只借用 key 指针），返回写了几条。调用方据此"宁可多送不可静默
 * 丢"：EdcInput 边编辑边删孤儿，提交路径可以在此基础上再兜一层。 */
int edc_payload_unreferenced(const char *text, const struct edc_payload *payload, const char **out, int cap);

/* Free a string this module returned (sds under the hood). NULL is a no-op. */
void edc_payload_string_free(char *string);

#ifdef __cplusplus
}
#endif

#endif /* __EDC_PAYLOAD_H__ */
