#ifndef __BC_SCHEDULER_H__
#define __BC_SCHEDULER_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_task.h"
#include "edc_task_group.h"
#include "sds.h"
#include <stdbool.h>

struct edc_session;
struct edc_config;
struct edc_channel_message;

struct edc_sched_task_entry;
/* edc_sched_task_entry is an internal hash entry used by uthash.
   Declared here for the hash head pointer type only. */

/* ── 回调类型 ──────────────────────────────────────────────── */

struct edc_scheduler;

/** 发送 LLM 请求: task 上下文已就绪，回调构造并发送给 llm_actor */
typedef void (*edc_scheduler_send_llm_fn)(struct edc_scheduler *sched, struct edc_task *task, void *request_id);

/** 发送 channel 回复 */
typedef void (*edc_scheduler_send_reply_fn)(struct edc_scheduler *sched, struct edc_task *task, const char *content,
                                            edc_channel_message_type message_type, void *request_id);

/** 发送 TOOL 类型 channel 消息 (带 header)；仅由 active task 触发。
 *  render_pref 由发送方(tool)声明，渲染端据此决定是否按 Markdown 渲染。
 *  payload 为可选结构化 JSON（工具原始参数等），随消息透传到渲染端；可为 NULL。 */
typedef void (*edc_scheduler_send_tool_fn)(struct edc_scheduler *sched, struct edc_task *task, const char *header,
                                           const char *content, edc_render_pref_t render_pref, const char *payload,
                                           void *request_id);

/** 分发 tool calls */
typedef void (*edc_scheduler_dispatch_tools_fn)(struct edc_scheduler *sched, struct edc_task *task, void *llm_resp,
                                                void *request_id);

/** 检查/处理命令 (返回 true=已处理) */
typedef bool (*edc_scheduler_command_fn)(struct edc_scheduler *sched, struct edc_actor *actor,
                                         struct edc_channel_message *msg);

/** 触发 memory compress */
typedef void (*edc_scheduler_memory_fn)(struct edc_scheduler *sched);

/** 保存 session */
typedef void (*edc_scheduler_save_fn)(struct edc_scheduler *sched);

/** task 树快照已就绪，交由上层 (agent) 回传给 TUI 的 channel adapter */
typedef void (*edc_scheduler_snapshot_fn)(struct edc_scheduler *sched, sds serialized);

/* ── 调度器 ────────────────────────────────────────────────── */

struct edc_scheduler
{
    struct edc_session *session;
    struct edc_config *config;

    struct edc_task_group *foreground_group;

    struct edc_sched_task_entry *task_hash; /* uthash: request_id → edc_task */

    edc_actor_t *self;

    /* 回调 (Phase 5 由 edc_agent 注入) */
    edc_scheduler_send_llm_fn send_llm;
    edc_scheduler_send_reply_fn send_reply;
    edc_scheduler_send_tool_fn send_tool_message;
    edc_scheduler_dispatch_tools_fn dispatch_tools;
    edc_scheduler_command_fn handle_command;
    edc_scheduler_memory_fn trigger_memory;
    edc_scheduler_save_fn save_session;
    edc_scheduler_snapshot_fn snapshot_ready;

    int64_t next_task_id;

    void *ctx; /* 回调上下文 (edc_agent*) */
};

/* ── API ───────────────────────────────────────────────────── */

extern struct edc_scheduler *edc_scheduler_new(struct edc_session *session, struct edc_config *config);
extern void edc_scheduler_free(struct edc_scheduler *sched);

extern edc_actor_t *edc_scheduler_get_actor(struct edc_scheduler *sched);

/* ── Task 管理 ─────────────────────────────────────────────── */

extern char *edc_scheduler_gen_task_id(struct edc_scheduler *sched);
extern struct edc_task *edc_scheduler_create_task(struct edc_scheduler *sched, const char *desc,
                                                  edc_channel_type channel_type);
extern struct edc_task_group *edc_scheduler_create_group(struct edc_scheduler *sched, struct edc_task *leader);
extern void edc_scheduler_add_group(struct edc_scheduler *sched, struct edc_task_group *group);

extern struct edc_task *edc_scheduler_find_task(struct edc_scheduler *sched, void *request_id);
extern struct edc_task *edc_scheduler_find_task_by_id(struct edc_scheduler *sched, const char *task_id);

/* ── 前台/后台 ─────────────────────────────────────────────── */

extern void edc_scheduler_set_foreground(struct edc_scheduler *sched, struct edc_task_group *group);
extern struct edc_task_group *edc_scheduler_get_foreground(struct edc_scheduler *sched);

/* ── 注册/反注册 ──────────────────────────────────────────────── */

extern void edc_scheduler_register_task(struct edc_scheduler *sched, struct edc_task *task, void *request_id);
extern void edc_scheduler_unregister_task(struct edc_scheduler *sched, void *request_id);

/* ── 信号 ──────────────────────────────────────────────────── */

extern void edc_scheduler_send_signal_to_task(struct edc_scheduler *sched, const char *task_id, edc_task_signal sig);
extern void edc_scheduler_send_signal_to_group(struct edc_scheduler *sched, const char *group_id, edc_task_signal sig);
extern void edc_scheduler_send_signal_to_all(struct edc_scheduler *sched, edc_task_signal sig);

#endif
