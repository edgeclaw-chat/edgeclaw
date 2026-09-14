#ifndef __EDC_RUNTIME_H__
#define __EDC_RUNTIME_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "edc_actor.h"
#include "edc_channel_actor.h"
#include "edc_config.h"
#include <stdbool.h>

/* ── 装配根 (composition root) ────────────────────────────────
 *
 * `struct edc_runtime` 把 config / session_manager / toolbox / scheduler / 各 actor /
 * runner 装到一起, 并把 scheduler 的 9 个 ops 注入好。**它只在这里可见**:
 *
 *   - 定义在 `edc_runtime.c` 里 (**不透明**) —— 外部拿不到字段, 也就没法顺手
 *     从装配根往下捎带;
 *   - **唯一需要它的外部读者是边界** (TUI 的 C bridge: 建/销毁、读 config、
 *     读活跃 session、给通道 actor 发消息) —— 经下面的访问器;
 *   - userland (命令层/shell) **不收它**: 它们收自己真正需要的那几件
 *     (`edc_commands_handler` 的签名就是名册)。 */
struct edc_runtime;

/* 原型里用到的标签必须**在这里**声明一次: 否则 GCC 会把参数表里的
 * `struct edc_scheduler` 当作 prototype-scope 的另一个类型, 定义处就报
 * "conflicting types" (类型打印出来还一模一样 —— 极难查)。 */
struct edc_actor;
struct edc_channel_message;
struct edc_config;
struct edc_scheduler;
struct edc_session_manager;

/* 命令处理器 (userland 入口) 的形状: 见 edc_commands.h 的实现约定。 */
typedef bool (*edc_runtime_command_fn)(struct edc_config *cfg, struct edc_session_manager *sm,
                                       struct edc_scheduler *sched, struct edc_reply_to to,
                                       struct edc_channel_message *message);

/* |sandbox_override|: -1 = keep the persisted config.json value (the default
 * at startup), 0 = force off, 1 = force on. The override is per-run and is NOT
 * persisted; only the in-TUI /sandbox command writes config.json. */
extern struct edc_runtime *edc_runtime_new(bool setup, int sandbox_override);
extern void edc_runtime_free(struct edc_runtime *rt);

/* ── 测试/嵌入式装配缝 ────────────────────────────────────────
 * 只把零件装上 —— **不**初始化环境、不建 actor、不启 runner, 也不接管任何零件的
 * 所有权 (config/session_manager/scheduler/channel_actor 仍归调用方)。测试要的
 * 正是"只挑它需要的那几件", 所以它不该再手工造一把伞。
 * |sched| 非空时它的 ctx 会被指向这个 runtime (回调上下文)。 */
extern struct edc_runtime *edc_runtime_attach(struct edc_config *cfg, struct edc_session_manager *sm,
                                              struct edc_scheduler *sched, edc_actor_t *channel_actor);

/* ── 边界访问器 (只服务上面那一个读者: TUI 的 C bridge) ──────── */
extern struct edc_config *edc_runtime_config(const struct edc_runtime *rt);
extern struct edc_session_manager *edc_runtime_sessions(const struct edc_runtime *rt);
extern edc_actor_t *edc_runtime_channel_actor(const struct edc_runtime *rt);

/* Update the LLM provider settings at runtime (from the TUI /connect menu):
 * persist api_url/model/api_key to config.json, update the in-memory config,
 * and refresh the llm_actor's copy. A NULL/empty argument leaves that field
 * unchanged. Returns true when the config was persisted. */
extern bool edc_runtime_set_llm_config(struct edc_runtime *rt, const char *api_url, const char *model,
                                       const char *api_key);

/* 注册命令处理器 (由 shell 模块在初始化时调用) */
extern void edc_runtime_set_command_handler(edc_runtime_command_fn fn);

#ifdef __cplusplus
}
#endif

#endif
