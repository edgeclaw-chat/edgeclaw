#ifndef __EDC_HTTPS_CLIENT_INTERNAL_H__
#define __EDC_HTTPS_CLIENT_INTERNAL_H__

#include <stdbool.h>

#include "sds.h"

/* Internals exposed for tests only -- not part of the client's API, and nothing outside
 * tests should include this header.
 *
 * They are here rather than static because redirect handling is the one part of this client
 * that can be decided without a socket: "is this status a redirect" and "what URL does this
 * Location mean" are pure functions, and testing them offline beats the alternative (a local
 * TLS server, or a live request to GitHub, in CI). */

/* 301/302/303/307/308. 304 is not a redirect: it validates the request already made. */
bool edc_https_status_is_redirect(int status);

/* Resolve a Location header against the URL that produced it. Returns NULL when the target
 * is not https (a downgrade is refused) or the base is unparseable. Caller frees. */
sds edc_https_resolve_redirect(const char *base_url, const char *location);

#endif /* __EDC_HTTPS_CLIENT_INTERNAL_H__ */
