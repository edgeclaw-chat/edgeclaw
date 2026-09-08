#ifndef __BC_AGENT_H__
#define __BC_AGENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_scheduler.h"
#include "edc_session.h"
#include "edc_session_manager.h"
#include "edc_toolbox.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_agent
{
    struct edc_config *config;
    struct edc_session_manager *session_manager;
    struct edc_toolbox *toolbox;
    struct edc_scheduler *scheduler;

    edc_actor_t *channel_actor;
    edc_actor_t *llm_actor;
    edc_actor_t *memory_actor;
    edc_actor_t *toolbox_actor;

    edc_actor_runner_t *runner;

    /* CLI history cache (for !n / !! expansion) */
    sds *history;
    int history_count;
    int history_capacity;
};

extern struct edc_agent *g_active_agent;

extern void edc_agent_add_history(struct edc_agent *agent, const char *line);

extern char *edc_agent_run_turn(struct edc_agent *agent, const char *input);
extern struct edc_agent *edc_agent_new(bool setup, bool enable_sandbox);
extern void edc_agent_remove_all_turns(struct edc_agent *agent);
extern void edc_agent_stop_all_turns(struct edc_agent *agent);
extern void edc_agent_free(struct edc_agent *agent);

/* 注册命令处理器 (由 shell 模块在初始化时调用) */
typedef bool (*edc_agent_command_fn)(struct edc_agent *, struct edc_actor *, struct edc_channel_message *);
extern void edc_agent_set_command_handler(edc_agent_command_fn fn);

#ifdef __cplusplus
}
#endif

#endif
