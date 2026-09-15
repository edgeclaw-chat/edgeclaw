#ifndef __BC_CHANNEL_ACTOR_H__
#define __BC_CHANNEL_ACTOR_H__

#include "cJSON.h"
#include "edc_actor.h"
#include "edc_config.h"
#include "sds.h"

/* ── 唯一的通道实现: adapter ──────────────────────────────────
 *
 * 多通道时代 (telegram / feishu / qq / cli / email) 的实现、注册表、以及
 * "按 channel 类型找 handler"那套机制**都已删除**: 今天只有一个 adapter,
 * 也就是 TUI 经 `edc_app` 的那条通路 (`edc_app_channel_adapter_init`)。
 * 于是 actor 不再持有 handlers 哈希, 只留一个发送回调。
 *
 * `edc_channel_type` 仍然只剩 `edc_channel_adapter` 这一个取值, 它继续挂在
 * task / message 上并且**写进磁盘** —— 那部分是格式兼容 (见 edc_channel_to_json),
 * 不是"还有别的通道"。 */

struct edc_channel_message
{
    sds content;
    sds header;     /* optional short description (e.g. executed command/tool name) shown above content */
    sds task_id;    /* id of the task that produced this message (NULL when the
                     * sender is not a task, e.g. user input or a snapshot).
                     * Rendering ends use it to label which task a chat message
                     * belongs to ("task 1"), so interleaved tasks stay readable. */
    sds session_id; /* id of the session that owns the producing task (NULL when
                     * the sender is not a task). Lets the frontend route a
                     * background session's output into *its own* transcript
                     * instead of the currently displayed one. */
    edc_channel_type type;
    edc_channel_message_type message_type;
    edc_render_pref_t render_pref; /* 发送方声明的渲染偏好；零值 PLAIN，未声明即纯文本 */
    bool from_active_task;         /* 发送方声明的事实: 这条产出**是不是它自己 session 的
                                    * active task 产生的**。纯声明 —— 内核不据此丢弃任何
                                    * 东西 (它不知道谁在看); 前端用它做展示闸门: 显示中的
                                    * session 只显示 active task 的产出, 其余静默
                                    * (P3 ③ 把这条规则从 core 移到了前端)。 */
};

typedef struct
{
    edc_actor_t *actor;
    edc_actor_t *observer;
    struct
    {
        void *instance;
        void (*on_actor_message_send_fn)(void *instance, struct edc_channel_message *msg);
    } adapter;
} edc_channel_actor_state_t;

extern edc_actor_t *edc_channel_actor_new(void);
extern void edc_channel_actor_free(edc_actor_t *actor);
extern void edc_channel_actor_set_observer(edc_actor_t *actor, edc_actor_t *observer);

/* ── adapter: 唯一通道的插口 ──────────────────────────────────
 * TUI 用它接收 `BC_ACTOR_MSG_CHANNEL_SEND` (要上屏的消息列表)。 */
extern void edc_channel_adapter_init(struct edc_actor *channel_actor, void *instance,
                                     void (*on_actor_message_send_fn)(void *, struct edc_channel_message *));

/* ── 磁盘格式兼容物: task / crontab job 的 `"channel"` 键 ─────
 * 老文件里有这个键, 新文件**照写不误** (去掉等于改 schema); 它的取值只可能是
 * adapter (唯一的通道), 所以读端直接忽略内容。 */
/* edc_channel_to_json() 已迁到 core/edc_config.h（落在 edc_channel_type 旁边）：
 * 落盘格式是底层关注, kernel 的 state_serializer 也要用它, 不该引边界模块的头。 */

/* ── 一条消息的去向: "投到哪条通道、以谁的名义、按哪种类型" ──────
 *
 * 这三件永远一起走 (出站投递要它们, userland 回话也要它们), 所以给它们一个
 * 名字 —— 而不是在几十个签名里各占一个位、还可能把顺序写反。 */
struct edc_reply_to
{
    struct edc_actor *channel_actor; /* 投给谁 (通道 actor) */
    struct edc_actor *actor;         /* 以谁的名义 (入站消息的发送者; 任务产出=scheduler actor) */
    edc_channel_message_type type;   /* 消息类型 (路由键) */
};

/* ── 出站: 投一条消息到通道 (前端) ───────────────────────────
 * |content| 的所有权归这条消息 (成功与失败都由这里接手)。 */
extern void edc_channel_publish(struct edc_reply_to to, const char *session_id, sds content);

/* "活跃 session 变成 X 了" (P3 ①b): 由命令层在 `/session switch|new` 成功后发出。 */
extern void edc_channel_notify_active_session(struct edc_actor *channel_actor, const char *session_id);

extern void edc_channel_message_free(struct edc_channel_message *msg);
extern struct edc_channel_message *edc_channel_message_new(edc_channel_type type, edc_channel_message_type message_type,
                                                           sds content);

/* ── 边界翻译: channel 消息 → core 的入站输入 ─────────────────
 *
 * 这里是"两个词表都认识"的地方 (channel 消息 + `struct edc_sched_input`),
 * 所以翻译归它 —— 依赖方向是"适配层 → core"。投递方 (frontend / 工具) 用它建负载,
 * 投 `BC_ACTOR_MSG_CHANNEL_RECV` 给 scheduler; scheduler 侧只认识后者。
 *
 * `edc_channel_input_new` **接管 `msg` 的所有权** (内容之外还负责在释放时
 * `edc_channel_message_free` 它), 所以它可以直接当 actor msg 的 data_free_fn 用:
 * `edc_actor_msg_new(..., edc_channel_input_new(msg), (edc_actor_data_free_fn)edc_channel_input_free)`。 */
struct edc_sched_input;
extern struct edc_sched_input *edc_channel_input_new(struct edc_channel_message *msg);
extern void edc_channel_input_free(struct edc_sched_input *input);

#endif
