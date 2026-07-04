#ifndef __BC_AGENT_H__
#define __BC_AGENT_H__

#include "edc_actor.h"
#include "edc_config.h"
#include "edc_session.h"
#include "edc_toolbox.h"
#include <cJSON.h>
#include <stdbool.h>

struct edc_agent
{
    struct edc_config *config;
    struct edc_session *session;
    struct edc_toolbox *toolbox;
    edc_actor_t *channel_actor;
    edc_actor_t *llm_actor;
    edc_actor_t *memory_actor;
    edc_actor_t *toolbox_actor;
    edc_actor_t *scheduler;
    edc_actor_runner_t *runner;

    struct edc_channel_turn *turns;
};

extern char *edc_agent_run_turn(struct edc_agent *agent, const char *input);
extern void edc_agent_run_loop(struct edc_agent *agent);
extern struct edc_agent *edc_agent_new(bool setup, bool enable_sandbox);
extern void edc_agent_remove_all_turns(struct edc_agent *agent);
extern void edc_agent_free(struct edc_agent *agent);

#endif
