#include "sds.h"
#ifndef __BC_TOOLBOX_H__
#define __BC_TOOLBOX_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_exec_io.h"
#include "edc_error.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_tool;
struct edc_task; /* fwd: 发起工具调用的 task，经 ctx->task 提供给工具 */

struct edc_toolbox
{
    struct edc_tool *tools;
    struct edc_config *conf;
    cJSON *schemas;
    edc_actor_t *scheduler;
    edc_actor_t *channel_actor;
};

struct edc_toolbox_call_ctx
{
    struct edc_toolbox *tb;
    edc_channel_type call_from_type;
    void *request_id; /* 发起该次工具调用的 task 的 request_id (由 toolbox actor 注入) */
    struct edc_task
        *task; /* 发起该次工具调用的 task (与 request_id 同源；agent 构建请求链时注入，非 task 路径为 NULL) */
    void *priv;

};

/* struct edc_toolbox_emit_msg 已移至 edc_exec_io.h (executor 的 I/O 词表)。 */

extern struct edc_toolbox_call_ctx *edc_toolbox_call_ctx_new(struct edc_toolbox *tb, edc_channel_type channel_type,
                                                             void *request_id, struct edc_task *task, void *priv);
extern void edc_toolbox_call_ctx_free(struct edc_toolbox_call_ctx *);
extern void edc_toolbox_emit_msg_free(void *p);
extern struct edc_toolbox *edc_toolbox_new(struct edc_config *conf, edc_actor_t *scheduler, edc_actor_t *channel_actor);
extern void edc_toolbox_free(struct edc_toolbox *tb);

extern void edc_toolbox_register(struct edc_toolbox *tb, struct edc_tool *tool);
extern void edc_toolbox_register_defaults(struct edc_toolbox *tb);

extern void edc_toolbox_exec(struct edc_toolbox_call_ctx *, const char *name, cJSON *args,
                             void (*callback)(struct edc_toolbox_call_ctx *, sds, EdcError *));

/* Request a TOOL-type channel message for the tool result (header = short
 * command/tool description, content = tool output). The request is routed
 * through the scheduler actor, which only forwards it when the originating
 * task is still the session's active task — so background tasks stay silent.
 *
 * render_pref 由发送方（工具）按输出内容显式声明，渲染端零猜测：
 *   - emit_tool_message_plain：纯文本输出（代码/命令输出/确认消息等），渲染端原样显示；
 *   - emit_tool_message_md：输出本身是 Markdown（如 checklist/表格），按 Markdown 渲染。
 * 消息经 scheduler 转发，仅在来源 task 仍为 active task 时显示——后台任务保持静默。 */
extern void edc_toolbox_emit_tool_message_plain(struct edc_toolbox_call_ctx *ctx, const char *header,
                                                const char *content);
extern void edc_toolbox_emit_tool_message_md(struct edc_toolbox_call_ctx *ctx, const char *header, const char *content);
#endif
