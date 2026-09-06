#ifndef EDC_TUI_AGENT_BRIDGE_H
#define EDC_TUI_AGENT_BRIDGE_H

/*
 * Thin C interface between the TUI (C++) and the agent module (C).
 *
 * The agent's real headers pull in sds.h, cJSON, uthash -- C headers that
 * don't compile under C++ (sds uses void* -> struct* implicit conversions).
 * Rather than patch third-party C headers, the TUI talks to the agent only
 * through this bridge: signatures use plain C types (const char*, size_t,
 * bool), never sds/cJSON. The bridge is implemented in the agent module (C),
 * which is free to use sds internally.
 *
 * Add a function here when the TUI needs a new agent capability, and implement
 * it in src/agent/edc_tui_agent_bridge.c (registered in src/agent/BUILD).
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct edc_agent;

/* Create / destroy an agent. The returned pointer is opaque to the TUI; it is
 * only passed back into other bridge functions. Mirrors edc_agent_new/free in
 * edc_agent.h but does not require the TUI to include that header. */
struct edc_agent *edc_tui_agent_new(bool setup, bool enable_sandbox);
void edc_tui_agent_free(struct edc_agent *agent);

#ifdef __cplusplus
}
#endif

#endif /* EDC_TUI_AGENT_BRIDGE_H */
