#ifndef EDC_ENDPOINT_PROBE_H
#define EDC_ENDPOINT_PROBE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A one-shot "is this OpenAI-compatible endpoint reachable with this key" probe, for the
 * first-run page's Test button.
 *
 * BLOCKING: it runs its own HTTPS client (which owns its own loop + thread) to
 * completion or |timeout_ms|, so call it OFF the UI/runner thread, or accept the brief
 * freeze. Writes a short human message into |out| and returns true when the endpoint
 * ANSWERED (any HTTP status, key valid or not -- a 401 still proves the URL + path are
 * right); false on a transport failure or timeout.
 */
extern bool edc_endpoint_probe(const char *cacert_path, const char *api_url, const char *api_key, int timeout_ms,
                               char *out, size_t cap);

#endif
