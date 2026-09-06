#include "edc_tui_agent_bridge.h"
#include "edc_agent.h"

/*
 * Bridge implementation. Lives in the agent module so it can see edc_agent.h
 * (and the sds/cJSON headers that pulls in); the TUI only sees the bridge
 * header, which uses plain C types.
 */

struct edc_agent *edc_tui_agent_new(bool setup, bool enable_sandbox)
{
    return edc_agent_new(setup, enable_sandbox);
}

void edc_tui_agent_free(struct edc_agent *agent)
{
    edc_agent_free(agent);
}
