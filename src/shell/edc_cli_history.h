#ifndef __EDC_CLI_HISTORY_H__
#define __EDC_CLI_HISTORY_H__

/* ── CLI 的 `!n` / `!!` 历史 ───────────────────────────────────
 *
 * 这是 **shell 自己的**命令历史: 它的**语义** (`!1` / `!!`、索引与边界) 整个在
 * `edc_shell.c` 的 `edc_shell_expand_history()` 里, 而**读它的也只有 shell**。
 * 进程一份、不落盘、与 session / channel 都无关 —— 同包 `edc_completion.c` 里
 * 有一份**同类**状态 (补全源的 Frecency 历史 `g_history`)。
 *
 * 它**不是 TUI 的历史**, 别跟这两个搞混 (三个 datum, 各有各的读者, 不要合并):
 *   · `src/tui/edc_input.cpp` 的 `history_` —— 输入框 Up/Down 的行召回
 *     (C++ 成员, 瞬态, 上限 500; 那个才是"前端界面的历史");
 *   · `edc_completion.c` 的 `g_history` —— 补全候选的排序来源 (2000 条, 带评分);
 *   · 本文件 —— `!n` / `!!` 展开用的命令历史。
 *
 * 沿革: 曾经挂在 `struct edc_runtime` 上 (于是 shell 的 `!` 展开要收一把伞、测试
 * 要为它造一个 agent), P4 卸到 `assembly/`, 2026-09-16 判定它归 shell 自己 →
 * 搬进 `src/shell/`。**方向性理由**: 将来真去**写**它的必然是 TUI (谁读到用户敲的
 * 行谁记录), 而 `tui → shell` 这条边**本来就存在**; 反过来把它放 tui, shell 就得
 * 向上伸手够 tui 的包, 那样必须加一个反向叶 target —— 需要那个 hack 本身就是
 * 放错位置的信号 (试过一版, 见 docs/runtime-model.md §11)。
 *
 * **既有偏离原样保留**: 生产路径里**没有任何地方写它** (唯一调用者是测试),
 * 所以真实使用中 `!n`/`!!` 永远展开不出东西。搬家不改行为 (§5.1 台账第 9 条)。
 *
 * 另一个历史上的怪处一并保留: 以前 `#define CLI_HISTORY_MAX 1000` 定义了却从
 * 来没有读者 (历史无上限) —— 那个宏随搬家删掉, 但**上限这件事仍然是"没有"**。 */

/** 记录一行 (空行忽略; 与上一行相同时忽略)。 */
extern void edc_cli_history_add(const char *line);

/** 已记录的行数。 */
extern int edc_cli_history_count(void);

/** 第 |index| 行 (0-based, 即 `!1` 是 at(0))。越界返回 NULL。 */
extern const char *edc_cli_history_at(int index);

/** 清空 (测试隔离用: 这份历史现在是进程级的)。 */
extern void edc_cli_history_clear(void);

#endif
