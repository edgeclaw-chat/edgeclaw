#ifndef __EDC_PASSES_H__
#define __EDC_PASSES_H__
/* AST 变换 pass 链 —— 流水线上"解析之后、解释执行之前"的那一阶段。
 *
 *   用户输入 → edc_lex → edc_classify → [edc_parse] → **passes** → edc_interp → task
 *
 * 约定（新增功能就加一个 pass，不要往解释器里塞）：
 *   - pass **只改 IR**：挂 synthetic 块、补依赖、给已解析的文本去转义。不建任务、不回话、
 *     不碰调度。
 *   - **任何会改变"这行是什么"的变换都不能住在这里**：这一层跑在 parse 之后，`has_var_refs`
 *     已经在词法期定死在 stage 上（edc_parse.y 的 stage_set_word），事后改 msg.text 既不会
 *     让依赖被收集，也不会让请求期去替换 `{{…}}`。这类变换属于**文本层**（edc_text_passes.h），
 *     跑在分类之前。
 *   - pass 的失败分两种：**就地记在载荷里**（例如某个 @文件读不到 —— 只写一行说明，输入
 *     照常执行），和**拒绝整条输入**（返回 false + out_error，解释器根本不跑）。默认前者。
 *   - 顺序＝注册顺序（edc_passes.c 的 k_passes[]），后面的 pass 看到前面 pass 的结果。
 *
 * 现有的 pass：@文件引用（edc_pass_mentions.c）。变量引用（{{…}} 预展开）是下一个候选：
 * 它现在还在 edc_interp_shell 的 stage 循环里。 */
#include "edc_ast.h"
#include "sds.h"
#include <stdbool.h>

struct edc_config;
struct edc_scheduler;
struct edc_session_manager;

/* pass 能看到的上下文。|workdir| 是这条输入所属 session 绑定的目录（相对路径按它解析）。 */
struct edc_pass_ctx
{
    struct edc_config *conf;
    struct edc_session_manager *sm;
    struct edc_scheduler *sched;
    const char *workdir;
};

/* 跑完所有 pass。true = 继续解释；false = 这条输入被拒，|*out_error| 是给用户看的原因。 */
bool edc_passes_run(const struct edc_pass_ctx *ctx, edc_ast *ast, sds *out_error);

/* ── 各个 pass ──────────────────────────────────────────────── */

/* `@path` 文件引用：把引用到的文件读成 synthetic 块（`payload->text` 里的 `@path` 原样
 * 保留 —— 模型看到的就是"用户写了 @path" + "read 工具的结果"，和它自己 read 一次等价）。
 *
 * 语法（与前端触发规则同一套）：
 *   - `@` 在行首或前一位是空白才是一次引用（`foo@bar.com`、`@Override` 里的不算）；
 *   - 引用一直延伸到下一个空白；尾巴上的 ASCII 标点先剥掉再解析（`看下 @a.c.`）；
 *   - `\@` 是转义：去掉反斜杠、不展开（`为什么 \@a.c 没展开`）；
 *   - `@path#12` / `@path#12-20` 是行范围（1 基，含两端；`#12-` = 到结尾）；
 *   - `@dir`（目录）展开成**一层清单**：每行 `[DIR]  name` / `[FILE] name`，不列 `.` 和
 *     `..`，最多 200 条（多了写 `… [truncated]`）；行范围对目录不适用，给了也不写进 header；
 *   - 同一载荷里重复的同一引用只展开一次。
 *
 * 单个引用失败（不存在、越沙箱、二进制、超预算）不会拒绝输入：synthetic 里写一行说明。 */
bool edc_pass_mentions(const struct edc_pass_ctx *ctx, edc_ast *ast, sds *out_error);

#endif
