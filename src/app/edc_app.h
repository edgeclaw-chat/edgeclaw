#ifndef __EDC_APP_H__
#define __EDC_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* ── 应用实例 (装配根 / composition root) ─────────────────────
 *
 * `struct edc_app` 把 config / session_manager / toolbox / scheduler / 各 actor /
 * runner 装到一起, 把 scheduler 的 ops 注入好, 并把 userland 的命令处理器
 * 装上 (`edc_app_new()` 里就地完成)。**它只在这里可见**:
 *
 *   - 定义在 `edc_app.c` 里 (**不透明**) —— 外部拿不到字段, 也就没法顺手
 *     从装配根往下捎带;
 *   - userland (命令层 / shell) **不认识它** —— 它们收自己真正需要的那几件
 *     (`edc_commands_handler` 的签名就是名册)。
 *
 * ── 本头只装装配根本职 ───────────────────────────────────────
 * 建 / 关 / 释放, 加 `set_llm_config` (它**是**业务: 写 config.json + 通知
 * llm_actor), 再加**两个**句柄访问器 (config / channel_actor) 与一批**命名查询** ——
 * 会碰 session 仓库的问题 (如"活跃会话是谁") 在这里由装配根自己回答: 仓库归
 * scheduler 所有 (`edc_scheduler_open` 建的那份), **边界拿答案, 不拿句柄**
 * (2026-09-22; 此前是 `edc_app_sessions()` 把句柄直接交出去)。
 * 前端面 (投输入 / 要树 / 注册 adapter / 读配置字符串 / 消息只读窗口) **不在这里**,
 * 在 `edc_api.h` (`//src/app:api`): 那批函数只是"C++ ↔ C 边界"上的投影,
 * 不是装配根的一部分 (2026-09-16 拆出)。
 *
 * ── 这个头对 C++ 安全 (2026-09-16) ───────────────────────────
 * 前端的 C++ 可以直接 include 它、直接调这里的函数。硬约束只有一条:
 * **不 include 任何拉 sds 的头** —— `sds.h` 的 `SDS_HDR_VAR` 宏在 C++ 下
 * 编不过 (`invalid conversion from 'void*' to 'sdshdr8*'`; cJSON/uthash 则没
 * 问题, 实测过)。所以:
 *   - 只前向声明标签, 签名只用 bool / int / char* / 不透明指针;
 *   - 凡是"sds → 调用方缓冲区"的读取, 都住在 `edc_api.h`。
 * 从前 TUI 侧那层 `edc_c_bridge` 里"纯转发"的一半, 就是被这一条消掉的。
 */
struct edc_app;

/* 原型里用到的标签必须**在这里**声明一次: 否则 GCC 会把参数表里的
 * `struct edc_scheduler` 当作 prototype-scope 的另一个类型, 定义处就报
 * "conflicting types" (类型打印出来还一模一样 —— 极难查)。 */
struct edc_actor;
struct edc_config;

/* |sandbox_override|: -1 = keep the persisted config.json value (the default
 * at startup), 0 = force off, 1 = force on. The override is per-run and is NOT
 * persisted; only the in-TUI /sandbox command writes config.json. */
extern struct edc_app *edc_app_new(bool setup, int sandbox_override);

/* 关进程 (幂等, 可重复调用): 停 actor/runner → 把所有 session **原样落盘** → 清掉在途 turn。
 * 它**会写盘** —— 名字里不带 "free" 就是为了这个: 销毁一个 app 不只是 free()。
 * 单独暴露它是为了让"先落地、后释放"成为两件可分的事 (TUI 退出路径仍只调 free)。 */
extern void edc_app_shutdown(struct edc_app *app);

/* 释放内存。会先补齐 `edc_app_shutdown()` 那一步 (保持"销毁即落盘"的老行为),
 * 然后依次 free scheduler / session_manager / config / toolbox。 */
extern void edc_app_free(struct edc_app *app);

/* ── 句柄访问器 = 装配根与 `//src/app:api` 的合同 ─────────────
 * app 是 opaque 的, 所以外面只能拿这几件"零件"; 前端面的那批投影函数就是
 * 全部经这里取数 (实测 `edc_api.c` 里 `app->` 为 0 次)。 */
extern struct edc_config *edc_app_config(const struct edc_app *app);
extern struct edc_actor *edc_app_channel_actor(const struct edc_app *app);

/* ── 命名查询 (边界拿"答案", 不拿句柄) ───────────────────────
 * 会碰 session 仓库的问题在这里由**装配根自己**回答 —— 仓库是 app 的私产
 * (edc_scheduler_open 建的那份), 不再把手柄交到边界外。契约同 api 侧的读口:
 * 成功才写 |buf| 并返回 true; 失败一律置空、返回 false。 */
extern bool edc_app_active_session_id(const struct edc_app *app, char *buf, size_t cap);

/* Update the LLM provider settings at runtime (from the TUI /connect menu):
 * persist api_url/model/api_key to config.json, update the in-memory config,
 * and refresh the llm_actor's copy. A NULL/empty argument leaves that field
 * unchanged. Returns true when the config was persisted. */
extern bool edc_app_set_llm_config(struct edc_app *app, const char *api_url, const char *model,
                                   const char *api_key);

#ifdef __cplusplus
}
#endif

#endif
