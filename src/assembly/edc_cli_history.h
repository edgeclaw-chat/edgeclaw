#ifndef __EDC_CLI_HISTORY_H__
#define __EDC_CLI_HISTORY_H__

/* ── CLI 的 `!n` / `!!` 历史 ───────────────────────────────────
 *
 * 这是**进程级的前端状态** (一次 CLI/TUI 一份), 不是运行时装配的一部分: 它既不
 * 属于 session store, 也不属于 channel。以前它挂在 `struct edc_runtime` 上, 于是
 * shell 的 `!` 展开要收一把伞、测试要为它手工造一个 agent —— 而真正需要它的
 * 只有 shell 自己。所以它现在归这里, 由 shell 直接读 (与 `edc_completion.c`
 * 的排序历史同一类状态: 前端自己的, 进程一份)。
 *
 * **既有偏离原样保留**: 生产路径里**没有任何地方写它** (唯一调用者是测试),
 * 所以真实使用中 `!n`/`!!` 永远展开不出东西。搬家不改行为 (§5.1 台账)。
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
