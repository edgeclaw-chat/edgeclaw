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
 * ── 这个头对 C++ 安全 (2026-09-16) ───────────────────────────
 * 前端的 C++ 可以直接 include 它、直接调这里的函数。硬约束只有一条:
 * **不 include 任何拉 sds 的头** —— `sds.h` 的 `SDS_HDR_VAR` 宏在 C++ 下
 * 编不过 (`invalid conversion from 'void*' to 'sdshdr8*'`; cJSON/uthash 则没
 * 问题, 实测过)。所以:
 *   - 只前向声明标签, 签名只用 bool / int / char* / 不透明指针;
 *   - 凡是"sds → 调用方缓冲区"的读取, 都做成 `...(char *buf, size_t cap)`
 *     形态的访问器 (见文件尾)。
 * 从前 TUI 侧那层 `edc_c_bridge` 里"纯转发"的一半, 就是被这一条消掉的。
 */
struct edc_app;

/* 原型里用到的标签必须**在这里**声明一次: 否则 GCC 会把参数表里的
 * `struct edc_scheduler` 当作 prototype-scope 的另一个类型, 定义处就报
 * "conflicting types" (类型打印出来还一模一样 —— 极难查)。 */
struct edc_actor;
struct edc_channel_message;
struct edc_config;
struct edc_session_manager;

/* |sandbox_override|: -1 = keep the persisted config.json value (the default
 * at startup), 0 = force off, 1 = force on. The override is per-run and is NOT
 * persisted; only the in-TUI /sandbox command writes config.json. */
extern struct edc_app *edc_app_new(bool setup, int sandbox_override);

/* 关进程 (幂等, 可重复调用): 停 actor/runner → 把所有 session **原样落盘** → 清掉在途 turn。
 * 它**会写盘** —— 名字里不带 "free" 就是为了这个: 销毁一个 app 不只是 free()。
 * 单独暴露它是为了让"先落地、后释放"成为两件可分的事 (TUI 退出路径仍只调 free)。 */
extern void edc_app_shutdown(struct edc_app *rt);

/* 释放内存。会先补齐 `edc_app_shutdown()` 那一步 (保持"销毁即落盘"的老行为),
 * 然后依次 free scheduler / session_manager / config / toolbox。 */
extern void edc_app_free(struct edc_app *rt);

/* ── 访问器 (前端拿得到的那几件) ─────────────────────────────── */
extern struct edc_config *edc_app_config(const struct edc_app *rt);
extern struct edc_session_manager *edc_app_sessions(const struct edc_app *rt);
extern struct edc_actor *edc_app_channel_actor(const struct edc_app *rt);

/* Update the LLM provider settings at runtime (from the TUI /connect menu):
 * persist api_url/model/api_key to config.json, update the in-memory config,
 * and refresh the llm_actor's copy. A NULL/empty argument leaves that field
 * unchanged. Returns true when the config was persisted. */
extern bool edc_app_set_llm_config(struct edc_app *rt, const char *api_url, const char *model,
                                   const char *api_key);

/* ── 通道消息的只读窗口 ───────────────────────────────────────
 * 消息本体的字段是 sds (或只有 C 侧才完整的类型), 所以前端只看这一组:
 * const char* 是**借用**, 生命周期 = 那条消息; 其余一律 int/bool。 */
extern const char *edc_app_message_content(const struct edc_channel_message *message);
extern const char *edc_app_message_header(const struct edc_channel_message *message);
extern const char *edc_app_message_task_id(const struct edc_channel_message *message);
extern const char *edc_app_message_session_id(const struct edc_channel_message *message);
extern int edc_app_message_is_user(const struct edc_channel_message *message);
extern int edc_app_message_is_tool(const struct edc_channel_message *message);
extern int edc_app_message_is_thought(const struct edc_channel_message *message);
extern int edc_app_message_is_resp(const struct edc_channel_message *message);
extern int edc_app_message_is_sys(const struct edc_channel_message *message);
extern int edc_app_message_is_task_snapshot(const struct edc_channel_message *message);
extern int edc_app_message_is_task_tree_changed(const struct edc_channel_message *message);
extern int edc_app_message_from_active_task(const struct edc_channel_message *message);
extern int edc_app_message_render_pref(const struct edc_channel_message *message);

/* 渲染偏好 (与 common 的 edc_render_pref_t 对应; 内联在这里以免 C++ 侧
 * include sds 系 C 头)。零值 = PLAIN: 发送方未声明一律按纯文本显示。
 * 对应关系由 `edc_app.c` 里的 _Static_assert 钉住, 不是"两份各自维护"。 */
enum
{
    EDC_APP_RENDER_PLAIN = 0,
    EDC_APP_RENDER_MARKDOWN = 1,
};

/* ── 配置 / 会话的字符串读取 (sds → 调用方缓冲区) ──────────────
 * 契约: 成功才写 |buf| 并返回 true; 失败一律把 |buf| 置空、返回 false。
 * |cap| = 缓冲区总字节数, 含结尾 NUL (装不下就不写)。 */
extern bool edc_app_config_model(const struct edc_app *rt, char *buf, size_t cap);
extern bool edc_app_config_api_url(const struct edc_app *rt, char *buf, size_t cap);
extern bool edc_app_config_api_key(const struct edc_app *rt, char *buf, size_t cap);
extern bool edc_app_sandbox_enabled(const struct edc_app *rt);
extern bool edc_app_active_session_id(const struct edc_app *rt, char *buf, size_t cap);
extern bool edc_app_active_session_workdir(const struct edc_app *rt, char *buf, size_t cap);
/* 指定 session 的转录文件路径 (`<session>/chat.jsonl`)。 */
extern bool edc_app_chat_log_path_for(const struct edc_app *rt, const char *session_id, char *buf, size_t cap);
/* 前端的命令历史文件 (`<bc_home>/.tui_history`)。 */
extern bool edc_app_history_path(char *buf, size_t cap);

/* ── 前端要做的几件事 ───────────────────────────────────────── */
/* 把前端注册成唯一通道实现的 adapter: 每一条要上屏的消息都到 |on_send|。 */
extern void edc_app_channel_adapter_init(struct edc_app *rt, void *instance,
                                         void (*on_send)(void *, struct edc_channel_message *));
/* 投一条用户输入; |session_id| 点名它属于哪个 session (NULL/"" = 默认活跃 session)。 */
extern void edc_app_send_user_message(struct edc_app *rt, const char *session_id, const char *content);
/* 要一棵 task 树 (P3 ①b): 树在 scheduler 线程上建, 以 TASK_SNAPSHOT 消息应答回来。 */
extern void edc_app_request_task_snapshot(struct edc_app *rt, const char *session_id);
/* 写一个**属于前端**的文件 (转录 / 命令历史): under_workspace 时路径必须落在
 * config.workspace 下, 且拒绝符号链接目标 (见 edc_file_fopen_safe)。 */
extern bool edc_app_write_file(struct edc_app *rt, const char *path, const char *data, size_t len,
                               bool append, bool under_workspace);
/* 建父目录; 路径必须落在 workspace 下 (与 edc_app_write_file 同一条规矩)。 */
extern bool edc_app_ensure_parent_dir(struct edc_app *rt, const char *path);

#ifdef __cplusplus
}
#endif

#endif
