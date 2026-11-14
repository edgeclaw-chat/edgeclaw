#ifndef __BC_EDC_PROXY_H__
#define __BC_EDC_PROXY_H__

#include "sds.h"

#include <stdbool.h>

/* HTTP CONNECT proxy support, decided from the environment.
 *
 * WHY HERE AND NOT IN THE HTTPS CLIENT: which proxy to use is a pure decision over strings --
 * environment value plus target host in, host/port/auth out -- and it has enough cases (no
 * scheme, user:pass, bare host, no_proxy with a leading dot, "*") to be worth testing without
 * a socket. The client asks this module once per request and then only has to know how to
 * speak CONNECT.
 *
 * WHAT IT DELIBERATELY DOES NOT DO: socks, and TLS to the proxy itself (`https://`). Both are
 * real configurations, but supporting them means a different handshake underneath this one, and
 * silently falling back to a direct connection when the user configured them would be worse
 * than refusing. edc_proxy_resolve() reports those through |why| so the caller can say what it
 * could not do. */

struct edc_proxy
{
    sds host; /* proxy hostname, owned */
    int port;
    sds auth; /* "Basic <base64>", ready to use as a header value; NULL when no credentials */
};

/* Fill |out| when the request to |target_host| should go through a proxy.
 *
 * Reads, in order: https_proxy, HTTPS_PROXY, all_proxy, ALL_PROXY. Honours no_proxy / NO_PROXY
 * (comma- or space-separated suffixes, "*" for everything, an optional leading dot).
 *
 * Returns false when no proxy applies. |why| (may be NULL) then receives a short, static
 * reason: NULL when nothing was configured, a sentence when something WAS configured but is
 * not usable -- which the caller should surface rather than silently going direct.
 *
 * |out| must be cleared with edc_proxy_clear() when the function returned true. */
bool edc_proxy_resolve(const char *target_host, struct edc_proxy *out, const char **why);

void edc_proxy_clear(struct edc_proxy *p);

#endif /* __BC_EDC_PROXY_H__ */
