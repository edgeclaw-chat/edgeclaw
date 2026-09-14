#ifndef __BC_COMMANDS_H__
#define __BC_COMMANDS_H__

#include "edc_channel_actor.h"
#include "edc_config.h"
#include <stdbool.h>
struct edc_actor;
struct edc_channel_message;
struct edc_config;
struct edc_scheduler;
struct edc_session_manager;

/* 命令处理器 (userland 入口)。收的是**它下游真正需要的**那几件 (形状 C:
 * 签名即读者名册) —— 不借任何伞对象: cfg 给 /model 这类显示、sm/sched 是
 * task 图与 store、channel_actor+actor+channel_type 是"回话地址"。 */
extern bool edc_commands_handler(struct edc_config *cfg, struct edc_session_manager *sm, struct edc_scheduler *sched,
                                 struct edc_reply_to to, struct edc_channel_message *);
extern void edc_commands_reply(struct edc_reply_to to, const char *text);

#endif
