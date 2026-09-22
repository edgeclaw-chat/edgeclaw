#ifndef __EDC_API_H__
#define __EDC_API_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* ── 前端的 C 安全面 (`//src/app:api`, 2026-09-16 从 edc_app.c 拆出) ──────────
 *
 * 这里装的不是业务, 是**投影**: `edc_app.h` 那边只管装配与生命周期 (建 / 关 /
 * 释放 / 交出句柄), 而"把 app 的状态投影成 C++ 看得懂的样子"是另一件事 —— 后者
 * 存在的唯一理由就是 **C++ ↔ C 的边界** (从前它住在 tui 的 `edc_c_bridge`;
 * 92b2c99 搬进 app; 现在单独成这个模块)。判据一句话: 本头里的函数**一条业务
 * 逻辑都不含** —— 投一条输入 / 要一棵树 / 注册 adapter / 读一个字符串。
 *
 * 与 `edc_app.h` 同一条硬约束: **对 C++ 安全** —— 只前向声明标签, 签名只用
 * bool / int / char* / 不透明指针, 不 include 任何拉 sds 的头 (`sds.h` 的
 * `SDS_HDR_VAR` 宏在 C++ 下编不过; cJSON/uthash 则没问题, 实测过)。所以:
 *   - sds 字段 → `...(char *buf, size_t cap)` 形态的读口 (见文件尾);
 *   - 消息字段 → 只读窗口 (`edc_app_message_*`)。
 *
 * 两个句柄访问器 (`edc_app_config` / `edc_app_channel_actor`, 都在 `edc_app.h` 里)
 * 就是本模块与装配根的**全部**合同: 实现里 `app->` 出现 **0 次**,
 * `struct edc_app` 仍然是只在 `edc_app.c` 里定义的不透明类型
 * (docs/runtime-model.md §8 判据 1)。会碰 session 仓库的问题这里**不问任何人** ——
 * 快照自己带着 session_id / workdir (2026-09-22)。 */
struct edc_app;
struct edc_channel_message;

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
extern int edc_app_message_is_task_history(const struct edc_channel_message *message);
extern int edc_app_message_is_task_snapshot(const struct edc_channel_message *message);
extern int edc_app_message_is_task_tree_changed(const struct edc_channel_message *message);
extern int edc_app_message_from_active_task(const struct edc_channel_message *message);
extern int edc_app_message_render_pref(const struct edc_channel_message *message);

/* 渲染偏好 (与 common 的 edc_render_pref_t 对应; 内联在这里以免 C++ 侧
 * include sds 系 C 头)。零值 = PLAIN: 发送方未声明一律按纯文本显示。
 * 对应关系由 `edc_api.c` 里的 _Static_assert 钉住, 不是"两份各自维护"。 */
enum
{
    EDC_APP_RENDER_PLAIN = 0,
    EDC_APP_RENDER_MARKDOWN = 1,
};

/* ── 配置 / 会话的字符串读取 (sds → 调用方缓冲区) ──────────────
 * 契约: 成功才写 |buf| 并返回 true; 失败一律把 |buf| 置空、返回 false。
 * |cap| = 缓冲区总字节数, 含结尾 NUL (装不下就不写)。 */
extern bool edc_app_config_model(const struct edc_app *app, char *buf, size_t cap);
extern bool edc_app_config_api_url(const struct edc_app *app, char *buf, size_t cap);
extern bool edc_app_config_api_key(const struct edc_app *app, char *buf, size_t cap);
extern bool edc_app_sandbox_enabled(const struct edc_app *app);
/* 指定 session 的转录文件路径 (`<workspace>/<session_id>/chat.jsonl`)。按约定拼出,
 * 不查仓库; |session_id| 必须是 id (快照/消息里带的那种), 显示名不再匹配。 */
extern bool edc_app_chat_log_path_for(const struct edc_app *app, const char *session_id, char *buf, size_t cap);
/* 前端的命令历史文件 (`<bc_home>/.tui_history`)。 */
extern bool edc_app_history_path(char *buf, size_t cap);

/* ── 前端要做的几件事 ───────────────────────────────────────── */
/* 把前端注册成唯一通道实现的 adapter: 每一条要上屏的消息都到 |on_send|。 */
extern void edc_app_channel_adapter_init(struct edc_app *app, void *instance,
                                         void (*on_send)(void *, struct edc_channel_message *));
/* 投一条用户输入; |session_id| 点名它属于哪个 session (NULL/"" = 默认活跃 session)。 */
extern void edc_app_send_user_message(struct edc_app *app, const char *session_id, const char *content);
/* 要一棵 task 树 (P3 ①b): 树在 scheduler 线程上建, 以 TASK_SNAPSHOT 消息应答回来。 */
extern void edc_app_request_task_snapshot(struct edc_app *app, const char *session_id);
/* 写一个**属于前端**的文件 (转录 / 命令历史): under_workspace 时路径必须落在
 * config.workspace 下, 且拒绝符号链接目标 (见 edc_file_fopen_safe)。 */
extern bool edc_app_write_file(struct edc_app *app, const char *path, const char *data, size_t len,
                               bool append, bool under_workspace);
/* 建父目录; 路径必须落在 workspace 下 (与 edc_app_write_file 同一条规矩)。 */
extern bool edc_app_ensure_parent_dir(struct edc_app *app, const char *path);

#ifdef __cplusplus
}
#endif

#endif
