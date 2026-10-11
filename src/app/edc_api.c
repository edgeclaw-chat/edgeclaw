/* 前端的 C 安全面 —— 从 `edc_app.c` 拆出来 (2026-09-16)。
 *
 * 这批函数**不是装配根的业务**: 它们是 "C++ ↔ C 边界" 上的投影 ——
 *   - `edc_app_message_*`: 消息字段是 sds, C++ 侧不能 include 那些头, 所以只读窗口在这里;
 *   - 字符串读口: 同一个理由 (sds → 调用方缓冲区);
 *   - 前端要做的几件事: 投输入 / 要快照 / 注册 adapter / 写它自己的文件。
 * 它们从前住在 tui 的 `edc_c_bridge` (92b2c99 搬进 app, 形状 C: 边界用伞), 现在单独
 * 成 `//src/app:api` —— 判据就一条: **这里一条业务逻辑都不含, 只是投影**。
 *
 * 唯一入参是 `struct edc_app *` (不透明): 本文件**不碰它的字段**, 只经
 * `edc_app_config()` / `edc_app_channel_actor()` 两个句柄访问器拿东西
 * (实测 `app->` = 0 次) ⇒ 装配根不为它开任何"内部头", `struct edc_app` 仍然
 * 只在 `edc_app.c` 里定义 (docs/runtime-model.md §8 判据 1 不退步)。
 *
 * 对 C++ 安全 (`edc_api.h` 与 `edc_app.h` 同规矩: 不 include 拉 sds 的头)。 */
#include "edc_api.h"

#include "edc_actor.h"
#include "edc_app.h"
#include "edc_channel_actor.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "log.h"
#include "sds.h"
#include <stdio.h>
#include <string.h>

/* ── 前端的只读窗口 (sds → 纯 C 类型) ─────────────────────────
 * 本文件对 C++ 安全 (不 include 任何拉 sds 的头), 所以凡是 sds 字段、
 * 或只有 C 侧才完整的内脏, 都要在这里过一道。这批函数从前住在 tui 的
 * `edc_c_bridge`; 搬家理由: **C++ ↔ C 的边界属于 app** (形状 C: 边界用伞,
 * 见 docs/runtime-model.md §11)。 */

_Static_assert(EDC_RENDER_MARKDOWN == EDC_APP_RENDER_MARKDOWN,
               "the EDC_APP_RENDER_* mirror in edc_api.h must match edc_render_pref_t");

const char *edc_app_message_content(const struct edc_channel_message *message)
{
    return message ? message->content : NULL;
}

const char *edc_app_message_header(const struct edc_channel_message *message)
{
    return message ? message->header : NULL;
}

/* 产出这条消息的 task id ("1"/"2"/...)；不是 task 发的 (用户输入、快照) 时为 NULL。 */
const char *edc_app_message_task_id(const struct edc_channel_message *message)
{
    return message ? message->task_id : NULL;
}

/* 产出这条消息的 task 属于哪个 session；不是 task 发的时为 NULL。
 * 前端据此把**别的** session 的后台产出记进那个 session 自己的转录。 */
const char *edc_app_message_session_id(const struct edc_channel_message *message)
{
    return message ? message->session_id : NULL;
}

int edc_app_message_is_user(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_USER_TYPE : 0;
}

int edc_app_message_is_tool(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_TOOL_TYPE : 0;
}

int edc_app_message_is_thought(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_THOUGHT_TYPE : 0;
}

int edc_app_message_is_resp(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_RESP_TYPE : 0;
}

int edc_app_message_is_sys(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_SYS_TYPE : 0;
}

/* `/task history` 的应答载荷 (结构化, 不是 SYS 文本): 前端认出它就按消息历史渲染。 */
int edc_app_message_is_task_history(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_TASK_HISTORY_TYPE : 0;
}

/* 一条拉取的**应答**: 内容是要解析的 task 树快照 JSON, 不许当聊天文本渲染。 */
int edc_app_message_is_task_snapshot(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE : 0;
}

/* 一个**事件**: "某个 session 的树变了" (只带 id + 原因, 不带树) —— P3 ①b。 */
int edc_app_message_is_task_tree_changed(const struct edc_channel_message *message)
{
    return message ? message->message_type == EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE : 0;
}

/* 发送方声明的事实: 这条产出是不是它自己 session 的 active task 产生的。
 * P3 ③ 起展示闸门在前端 —— 前端按"我正在显示哪个 session"决定显不显示。 */
int edc_app_message_from_active_task(const struct edc_channel_message *message)
{
    return (message && message->from_active_task) ? 1 : 0;
}

int edc_app_message_render_pref(const struct edc_channel_message *message)
{
    /* 返回 EDC_APP_RENDER_PLAIN/MARKDOWN；发送方未声明时为 PLAIN（零值默认） */
    return message ? (int)message->render_pref : EDC_APP_RENDER_PLAIN;
}

/* ── 配置 / 会话的字符串读取 (sds → 调用方缓冲区) ─────────────── */

static bool copy_sds_into(const sds value, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    buf[0] = '\0';
    if (!value)
        return false;
    const bool ok = sdslen(value) < cap;
    if (ok)
        memcpy(buf, value, sdslen(value) + 1);
    return ok;
}

bool edc_app_config_model(const struct edc_app *app, char *buf, size_t cap)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg ? copy_sds_into(cfg->llm_api_conf.model, buf, cap) : copy_sds_into(NULL, buf, cap);
}

bool edc_app_config_api_url(const struct edc_app *app, char *buf, size_t cap)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg ? copy_sds_into(cfg->llm_api_conf.api_url, buf, cap) : copy_sds_into(NULL, buf, cap);
}

bool edc_app_config_api_key(const struct edc_app *app, char *buf, size_t cap)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg ? copy_sds_into(cfg->llm_api_conf.api_key, buf, cap) : copy_sds_into(NULL, buf, cap);
}

bool edc_app_sandbox_enabled(const struct edc_app *app)
{
    struct edc_config *cfg = edc_app_config(app);
    return cfg && cfg->enable_sandbox;
}

/* 转录文件在哪 —— 只解路径: 编解码、追加、回放都是前端自己的事 (见
 * edc_chat_transcript.hpp)。
 *
 * 路径由**约定**拼出, 不再问仓库: 会话目录就是 `<workspace>/<session_id>`
 * (kernel 侧 edc_session_new_with_workdir 用的同一条规则, tests/test_session_workdir.c
 * 钉住它)。这样前端面不必碰 session 仓库 —— 它由 actor 线程改。
 *
 * |session_id| 必须是**会话 id**: 从前走 manager_find 时显示名也能匹配, 而两个
 * 调用点传的都是快照/消息里的 id, 所以那条便利没有被用上。 */
bool edc_app_chat_log_path_for(const struct edc_app *app, const char *session_id, char *buf, size_t cap)
{
    if (buf && cap > 0)
        buf[0] = '\0';
    struct edc_config *cfg = edc_app_config(app);
    if (!cfg || !cfg->workspace || sdslen(cfg->workspace) == 0 || !session_id || !*session_id || !buf || cap == 0)
        return false;

    sds path = edc_path_join(cfg->workspace, session_id, "chat.jsonl", NULL);
    const bool ok = path && strlen(path) < cap;
    if (ok)
        memcpy(buf, path, strlen(path) + 1);
    if (path)
        sdsfree(path);
    return ok;
}

/* 前端的命令历史文件 (`<bc_home>/.tui_history`) —— 是前端自己的进程级历史。 */
bool edc_app_history_path(char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    buf[0] = '\0';

    sds home = edc_config_resolve_bc_home();
    if (!home)
        return false;
    sds path = edc_path_join(home, ".tui_history", NULL);
    sdsfree(home);
    if (!path)
        return false;

    const bool ok = sdslen(path) < cap;
    if (ok)
        memcpy(buf, path, sdslen(path) + 1);
    sdsfree(path);
    return ok;
}

/* ── 前端要做的几件事 ───────────────────────────────────────── */

/* 把前端注册成**唯一**通道实现的 adapter: 每一条要上屏的消息都到 |on_send|。 */
void edc_app_channel_adapter_init(struct edc_app *app, void *instance,
                                  void (*on_send)(void *, struct edc_channel_message *))
{
    edc_actor_t *channel = edc_app_channel_actor(app);
    if (!channel)
        return;
    edc_channel_adapter_init(channel, instance, on_send);
}

/* 投一条用户输入。|session_id| 点名这条输入属于哪个 session (P3 ①a):
 * 它派出的 task 落在那儿, 不会去别处。NULL/"" = 未指定, 由 store 取默认。 */
void edc_app_send_user_message(struct edc_app *app, const char *session_id, const char *content)
{
    edc_actor_t *channel = edc_app_channel_actor(app);
    if (!channel)
        return;

    struct edc_channel_message *message =
        edc_channel_message_new(edc_channel_adapter, EDC_CHANNEL_MESSAGE_USER_TYPE, sdsnew(content));
    if (session_id && *session_id)
        message->session_id = sdsnew(session_id);

    edc_channel_actor_state_t *state = channel->state;
    /* 边界翻译: core 不认识 channel 消息 —— 翻译好再投 (edc_channel_actor.h)。 */
    edc_actor_msg_t *actor_msg =
        edc_actor_msg_new(BC_ACTOR_MSG_CHANNEL_RECV, channel, NULL, edc_channel_input_new(message),
                          (edc_actor_data_free_fn)edc_channel_input_free);
    edc_actor_send(state->observer, actor_msg);
}

/* 要一棵 task 树 (P3 ①b: 前端只拉自己正在显示的那一棵); 树在 scheduler 线程上建,
 * 经通道 adapter 以 TASK_SNAPSHOT 应答回来。 */
void edc_app_request_task_snapshot(struct edc_app *app, const char *session_id)
{
    edc_actor_t *channel = edc_app_channel_actor(app);
    if (!channel)
        return;

    edc_channel_actor_state_t *state = channel->state;
    if (!state || !state->observer)
        return;

    sds tag = sdsnew(session_id ? session_id : "");
    edc_actor_msg_t *msg = edc_actor_msg_new(BC_ACTOR_MSG_TASK_SNAPSHOT, channel, NULL, tag,
                                             (edc_actor_data_free_fn)sdsfree);
    edc_actor_send(state->observer, msg);
}

bool edc_app_ensure_parent_dir(struct edc_app *app, const char *path)
{
    if (!path)
        return false;

    sds parent = edc_path_dirname(path);
    if (!parent || !parent[0])
    {
        if (parent)
            sdsfree(parent);
        return false;
    }

    /* 只允许在 workspace 里面: 前端经这里写的都是它自己的文件。 */
    const struct edc_config *cfg = edc_app_config(app);
    const char *root = cfg ? cfg->workspace : NULL;
    bool ok = root && root[0] && strncmp(parent, root, strlen(root)) == 0;
    if (ok)
    {
        EdcError *err = NULL;
        ok = edc_dir_create_with_parents(parent, 0755, &err);
        if (err)
        {
            log_warn("edc_app_ensure_parent_dir: mkdir %s: %s", parent, err->message);
            edc_error_free(err);
        }
    }
    sdsfree(parent);
    return ok;
}

bool edc_app_write_file(struct edc_app *app, const char *path, const char *data, size_t len, bool append,
                        bool under_workspace)
{
    if (!path || (!data && len > 0))
        return false;

    const struct edc_config *cfg = edc_app_config(app);
    const char *root = (under_workspace && cfg) ? cfg->workspace : NULL;
    FILE *f = edc_file_fopen_safe(path, append ? "a" : "w", root);
    if (!f)
        return false;

    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    fclose(f);
    return ok;
}
