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

/* What a 3xx means for the caller. See the definition for why a 3xx without a Location is
 * delivered rather than failed. */
typedef enum
{
    EDC_HTTPS_REDIRECT_NONE = 0,
    EDC_HTTPS_REDIRECT_FOLLOW,
    EDC_HTTPS_REDIRECT_FAIL
} edc_https_redirect_action;

edc_https_redirect_action edc_https_redirect_decide(int status, bool has_location, int redirects_left);

/* Resolve a Location header against the URL that produced it. Returns NULL when the target
 * is not https (a downgrade is refused) or the base is unparseable. Caller frees. */
sds edc_https_resolve_redirect(const char *base_url, const char *location);

#endif /* __EDC_HTTPS_CLIENT_INTERNAL_H__ */
