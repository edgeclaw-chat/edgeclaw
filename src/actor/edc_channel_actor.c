#include "edc_channel_actor.h"
#include "edc_actor.h"
#include "edc_compat.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "log.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>

/* 唯一的通道 actor: `edc_channel_actor_bind_scheduler` 登记 (见文件末"端口"一节)。
 * 放在这里是因为 `edc_channel_actor_free` 要清它 —— 免得释放后还留着一个野指针。 */
static edc_actor_t *g_channel = NULL;

/* ── 边界翻译: channel 消息 → core 的入站输入 ─────────────────
 *
 * `struct edc_sched_input` 是 core 的词表 (edc_scheduler.h); 本文件是"两个词表都
 * 认识"的地方 —— 于是 core 不必知道 channel 消息的形状, 也不用 include 本头文件。
 * 输入**接管消息所有权**: 释放输入时顺带释放消息 (`origin` 只是透传, 但它的
 * 生命周期由这里管 —— 否则要么泄漏, 要么调度器得认识它的类型)。 */
struct edc_sched_input *edc_channel_input_new(struct edc_channel_message *msg)
{
    if (!msg)
        return NULL;
    struct edc_sched_input *input = calloc(1, sizeof(*input));
    if (!input)
    { /* OOM: 这条输入丢弃 (scheduler 对 NULL 负载直接返回), 不泄漏消息 */
        edc_channel_message_free(msg);
        return NULL;
    }
    input->session_id = msg->session_id;
    input->content = msg->content;
    input->type = msg->type;
    input->origin = msg;
    return input;
}

void edc_channel_input_free(struct edc_sched_input *input)
{
    if (!input)
        return;
    struct edc_channel_message *msg = input->origin;
    free(input);
    if (msg)
        edc_channel_message_free(msg);
}

/* 只有一个通道 (adapter): 出站消息 (CHANNEL_SEND) 直接交给那个回调。
 * 以前这里按 `msg->sub_type` (= channel 类型) 在 handlers 哈希里找实现 ——
 * 那是多通道的机制, 随通道实现一起删了。 */
static void edc_channel_actor_handle(edc_actor_t *actor, edc_actor_msg_t *msg)
{
    edc_channel_actor_state_t *state = actor->state;

    if (msg->type == BC_ACTOR_MSG_CHANNEL_SEND && state->adapter.on_actor_message_send_fn)
        state->adapter.on_actor_message_send_fn(state->adapter.instance, msg->data);

    edc_actor_msg_free(msg);
}

void edc_channel_adapter_init(edc_actor_t *channel_actor, void *instance,
                              void (*on_actor_message_send_fn)(void *, struct edc_channel_message *))
{
    edc_channel_actor_state_t *state = channel_actor->state;
    state->adapter.instance = instance;
    state->adapter.on_actor_message_send_fn = on_actor_message_send_fn;
}

struct edc_channel_message *edc_channel_message_new(edc_channel_type type, edc_channel_message_type message_type,
                                                    sds content)
{
    struct edc_channel_message *message = g_new0(struct edc_channel_message, 1);
    message->type = type;
    message->message_type = message_type;
    message->content = content;
    return message;
}

void edc_channel_message_free(struct edc_channel_message *msg)
{
    if (!msg)
        return;
    if (msg->header)
        sdsfree(msg->header);
    if (msg->task_id)
        sdsfree(msg->task_id);
    if (msg->session_id)
        sdsfree(msg->session_id);
    sdsfree(msg->content);
    free(msg);
}

edc_actor_t *edc_channel_actor_new(void)
{
    edc_channel_actor_state_t *state = calloc(1, sizeof(edc_channel_actor_state_t));

    state->observer = NULL;

    edc_actor_t *actor = edc_actor_new("edc_channel_actor", edc_channel_actor_handle, state);
    state->actor = actor;

    state->adapter.instance = NULL;
    state->adapter.on_actor_message_send_fn = NULL;
    return actor;
}

void edc_channel_actor_free(edc_actor_t *actor)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;

    if (g_channel == actor) /* 端口实现者的登记位: 别留野指针 */
        g_channel = NULL;
    free(state);
    edc_actor_free(actor);
}

void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer)
{
    edc_channel_actor_state_t *state = (edc_channel_actor_state_t *)actor->state;
    state->observer = observer;
}

/* ── 出站: 消息 → 通道 (前端) ─────────────────────────────────
 *
 * 建一条 channel 消息、发 `BC_ACTOR_MSG_CHANNEL_SEND` 给本 actor —— 这是**通道
 * 自己的**词汇 (消息形状 + 发送路径都归这里)。以前它住在 edc_agent.c, 只因为
 * 那里恰好握有 channel_actor; 现在任何持有通道句柄的模块都能投递。 */

/* 唯一的发送出口: publish 与两个 send_task_* 都从这里出去, 所以
 * `BC_ACTOR_MSG_CHANNEL_SEND` 全文件只出现一次。 */
static void edc_channel_send_msg(struct edc_reply_to to, struct edc_channel_message *msg, void *request_id)
{
    edc_actor_send(to.channel_actor,
                   edc_actor_msg_new_sub_type(BC_ACTOR_MSG_CHANNEL_SEND, edc_channel_adapter, to.actor, request_id, msg,
                                              (edc_actor_data_free_fn)edc_channel_message_free));
}

void edc_channel_publish(struct edc_reply_to to, const char *session_id, sds content)
{
    if (!to.channel_actor)
    {
        if (content)
            sdsfree(content);
        return;
    }
    struct edc_channel_message *msg = edc_channel_message_new(edc_channel_adapter, to.type, content);
    if (session_id && *session_id)
        msg->session_id = sdsnew(session_id);

    edc_channel_send_msg(to, msg, NULL);
}

/* ── 一条 task 产出 → 通道消息 ───────────────────────────────
 *
 * 属主 session: 被切到后台的 session 的产出照发, 由前端按 session_id 静默写进
 * 它自己的显示流。P3 ③: **"显示中的 session 只显示 active task" 这条展示规则
 * 移到了前端** —— core 只把事实声明出去 (from_active_task), 不再问"谁在看"
 * (它已经不知道了: 那份"当前 session"的副本已删)。 */
void edc_channel_send_task_reply(struct edc_reply_to to, struct edc_task *task, const char *content, void *request_id)
{
    if (!to.channel_actor || !task || !content)
        return;

    struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
    if (!own)
    {
        log_warn("dropping %s from task %s: no owning session", "reply", task->task_id ? task->task_id : "?");
        return;
    }

    struct edc_channel_message *reply = edc_channel_message_new(task->channel_type, to.type, sdsnew(content));
    /* Tag the message with its producing task so the frontend can show which
     * task a chat message belongs to (interleaved tasks in one session). */
    if (task->task_id)
        reply->task_id = sdsnew(task->task_id);
    /* And with its owning session, so a background session's output lands in
     * that session's transcript rather than the one on screen. */
    reply->session_id = sdsnew(own->session_id);
    /* 事实声明: 这是它 session 的 active task 吗 (前端据此做展示闸门)。 */
    reply->from_active_task = edc_session_get_active_task(own) == task;
    /* 发送方显式声明渲染偏好，渲染端(CLI/TUI)不再猜测：
     * 只有 LLM 回复 (RESP) 声明按 Markdown 渲染；思考/系统等内部文本保持
     * 默认 PLAIN（零值），原样显示。 */
    if (to.type == EDC_CHANNEL_MESSAGE_RESP_TYPE)
        reply->render_pref = EDC_RENDER_MARKDOWN;

    edc_channel_send_msg(to, reply, request_id);
}

/* 带 header 的 TOOL 消息。展示闸门 (只显示显示中的 session 的 active task) 在前端
 * —— 这里只声明事实 (from_active_task)。 */
void edc_channel_send_task_tool_message(struct edc_reply_to to, struct edc_task *task, const char *header,
                                        const char *content, edc_render_pref_t render_pref, void *request_id)
{
    if (!to.channel_actor || !task || !content)
        return;

    struct edc_channel_message *reply =
        edc_channel_message_new(task->channel_type, EDC_CHANNEL_MESSAGE_TOOL_TYPE, sdsnew(content));
    reply->render_pref = render_pref;
    if (task->task_id)
        reply->task_id = sdsnew(task->task_id);
    /* Owning session (see edc_channel_send_task_reply): routes a background
     * session's tool output into its own transcript. */
    {
        struct edc_session *own = (task->group && task->group->session) ? task->group->session : NULL;
        if (own && own->session_id)
            reply->session_id = sdsnew(own->session_id);
        reply->from_active_task = own && edc_session_get_active_task(own) == task;
    }
    if (header && *header)
        reply->header = sdsnew(header);

    edc_channel_send_msg(to, reply, request_id);
}

/* 与 scheduler 的 EDGECLAW_SNAPSHOT_TRACE 同一份 trace (同一条通路的另一半: 切台
 * 事件不走 scheduler 的脏表, 而是由这里直发) —— 证据工具要能看到它, 否则"前端怎么
 * 知道换台了"在 trace 里是断的。 */
static void edc_channel_trace_active_session(const char *session_id)
{
    const char *path = getenv("EDGECLAW_SNAPSHOT_TRACE");
    if (!path || !*path)
        return;
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "notify reason=active session=%s\n", session_id ? session_id : "?");
    fclose(f);
}

/* 活跃 session 变了 (用户 `/session switch|new`): 告诉前端换显示那棵树。
 *
 * 这是**事实**的通报 —— "active_index 现在是 X" (规则 3 的唯一权威在 session_manager),
 * 由 userland (命令层) 在切成功之后发出。不是"内核决定谁看什么": 前端仍然自己
 * 决定要不要跟 (它只是被叫去显示用户刚点的那个 tab), 快照也仍然要它自己拉。 */
void edc_channel_notify_active_session(struct edc_actor *channel_actor, const char *session_id)
{
    edc_channel_trace_active_session(session_id);
    /* 切台通报: 由通道以**自己的**名义发出 (没有入站发送者)。*/
    struct edc_reply_to to = {.channel_actor = channel_actor, .actor = channel_actor,
                              .type = EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE};
    edc_channel_publish(to, session_id, sdsnew("active"));
}

/* ── 端口: 通道是四条出站端口的提供方 ────────────────────────
 *
 * 消息形状与投递路径已经归本文件 (见上), 于是"把端口装到 scheduler 上"也归
 * 这里: 装配根只在装配期叫一次 `edc_channel_actor_bind_scheduler`, 运行期不再
 * 出现在这条路径上。端口签名一个字没动, kernel 的调用点与测试里的假端口都不
 * 受影响。
 *
 * 通道 actor 是**进程里唯一的一个** (1072c70 之后没有多通道了), 所以实现者
 * 在 bind 时登记自己即可 —— 不必往 kernel 的 scheduler 上挂一个"通道句柄"
 * (那会让 core 重新认识边界上的东西)。`.actor` 仍是 scheduler actor: 任务产出
 * 以调度器名义发出, 与从前装配根的 thunk 逐字一致。 */
static struct edc_reply_to edc_channel_to(struct edc_scheduler *sched, edc_channel_message_type type)
{
    struct edc_reply_to to = {.channel_actor = g_channel, .actor = edc_scheduler_get_actor(sched), .type = type};
    return to;
}

static void edc_channel_cb_send_reply(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                                      edc_channel_message_type message_type, void *request_id)
{
    edc_channel_send_task_reply(edc_channel_to(sched, message_type), task, content, request_id);
}

static void edc_channel_cb_send_tool_message(struct edc_scheduler *sched, struct edc_task *task, const char *header,
                                             const char *content, edc_render_pref_t render_pref, void *request_id)
{
    edc_channel_send_task_tool_message(edc_channel_to(sched, EDC_CHANNEL_MESSAGE_TOOL_TYPE), task, header, content,
                                       render_pref, request_id);
}

static void edc_channel_cb_snapshot_reply(struct edc_scheduler *sched, sds serialized)
{
    if (!serialized)
        return;
    edc_channel_publish(edc_channel_to(sched, EDC_CHANNEL_MESSAGE_TASK_SNAPSHOT_TYPE), NULL, serialized);
}

static void edc_channel_cb_snapshot_changed(struct edc_scheduler *sched, const char *session_id, const char *reason)
{
    edc_channel_publish(edc_channel_to(sched, EDC_CHANNEL_MESSAGE_TASK_TREE_CHANGED_TYPE), session_id,
                        sdsnew(reason ? reason : ""));
}

void edc_channel_actor_bind_scheduler(edc_actor_t *actor, struct edc_scheduler *sched)
{
    if (!actor || !sched)
        return;

    g_channel = actor;
    sched->send_reply = edc_channel_cb_send_reply;
    sched->send_tool_message = edc_channel_cb_send_tool_message;
    sched->snapshot_reply = edc_channel_cb_snapshot_reply;
    sched->snapshot_changed = edc_channel_cb_snapshot_changed;
}
