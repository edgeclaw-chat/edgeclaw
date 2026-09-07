#ifndef EDC_C_BRIDGE_H
#define EDC_C_BRIDGE_H

/*
 * Thin C interface between the TUI (C++) and the rest of the project (C).
 *
 * The project's C headers pull in sds.h, cJSON, uthash -- C headers that
 * don't compile under C++ (sds uses void* -> struct* implicit conversions).
 * Rather than patch third-party C headers, the TUI talks to the project only
 * through this bridge: signatures use plain C types (const char*, size_t,
 * bool), never sds/cJSON. The bridge is implemented in edc_c_bridge.c (compiled
 * as C, so it can include sds-based headers freely).
 *
 * Add a function here when the TUI needs a new C-side capability, and
 * implement it in edc_c_bridge.c. This is a general C API surface, not
 * agent-specific: prefix all functions with edc_c_.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct edc_agent;
struct edc_actor;
struct edc_channel_message;
struct uv_loop_s;

/* Create / destroy an agent. The returned pointer is opaque to the TUI; it is
 * only passed back into other bridge functions. Mirrors edc_agent_new/free. */
struct edc_agent *edc_c_agent_new(bool setup, bool enable_sandbox);
void edc_c_agent_free(struct edc_agent *agent);

/* The agent's channel actor, so the TUI can register itself as an adapter
 * channel (see edc_channel_adapter_init). Opaque pointer -- the TUI only
 * passes it back into C functions that take struct edc_actor*. */
struct edc_actor *edc_c_agent_channel_actor(struct edc_agent *agent);
const char *edc_c_channel_message_content(struct edc_channel_message *);
/* Optional short description carried by a message (e.g. the executed command
 * for tool messages). May return NULL when the sender provided no header. */
const char *edc_c_channel_message_header(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_user(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_tool(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_thought(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_resp(struct edc_channel_message *channel_message);
int edc_c_channel_message_is_sys(struct edc_channel_message *channel_message);

void edc_c_file_utils_init(struct uv_loop_s *loop);
void edc_c_agent_send_user_message(struct edc_agent *agent, const char *content);

#ifdef __cplusplus
}
#endif

#endif /* EDC_C_BRIDGE_H */
