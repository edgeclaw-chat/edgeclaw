#ifndef __EDC_TEXT_PASSES_H__
#define __EDC_TEXT_PASSES_H__
/* 文本层 pass 链 —— 流水线上"分类之前"的那一阶段。
 *
 *   用户输入（字节） → **text passes** → edc_commands_input_kind → edc_lex → edc_parse
 *                     → [AST passes] → edc_interp → task → 请求期 {{…}}
 *
 * 为什么需要这一层（这是它的全部意义）：
 *   AST pass 跑在 parse 之后，只能改 IR。**它改不回"这行是什么"** —— 词法期算好的
 *   结果（`has_var_refs`，见 edc_lex.l 与 edc_parse.y 的 stage_set_word）已经定死在
 *   stage 上，事后改 `msg.text` 也补不回来：edc_interp 的 `if (s->has_var_refs)` 会
 *   直接跳过依赖收集，请求期的 `{{…}}` 替换也不会发生。所以凡是会改变"这行是什么"
 *   的变换（例如把粘贴占位符换成正文，正文首字符可能是 `/` `+` `!`，也可能引着
 *   `{{LAST}}`），只能住在这里。
 *
 * 契约（新增一个 text pass 必须守住这三条）：
 *   1. **纯函数**：不读盘、不查 sched/session、不建任务、不写日志副作用。分类依赖它，
 *      分类就必须可预测；有 I/O 的变换属于 AST 层（@文件引用就是）。
 *   2. **幂等**：同一个输入调两次结果相同。UI 每帧都会调一次（输入框上分割线的
 *      "这行会被怎么读"徽标），app 侧派发前还会再调一次，两边必须给出同一个答案。
 *   3. **不做语法**：不加引号、不转义、不解析。只做"占位符 → 内容"这类纯替换；
 *      其余一律留给 lexer。
 *
 * 顺序＝注册顺序（edc_text_passes.c 的 k_text_passes[]），后面的 pass 看到前面 pass
 * 的产物。目前只有一条：粘贴占位符展开（edc_pass_paste）。 */
#include "edc_payload.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 跑完整条文本层链，返回**定稿文本**（新串，edc_payload_string_free 释放；头文件不引
 * sds.h 是因为它也被 C++ 侧包含）。|raw| 为 NULL 时返回空串；没有载荷时它就是 |raw|
 * 的拷贝。永远成功：文本层没有"拒绝这条输入"这种失败 —— 拒绝是 AST 层的事（见
 * edc_passes.h）。 */
char *edc_text_passes_run(const char *raw, const struct edc_payload *payload);

/* 把 `text` 里的粘贴占位符换成正文（edc_payload_expand）。单遍、不递归：换进去的
 * 正文不再被扫描，所以粘贴内容里的 `@file` / `{{…}}` 都还是普通字节。 */
char *edc_pass_paste(const char *text, const struct edc_payload *payload);

#ifdef __cplusplus
}
#endif

#endif /* __EDC_TEXT_PASSES_H__ */
