#ifndef __EDC_UPDATE_SERVICE_H__
#define __EDC_UPDATE_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>

#include <uv.h>

#include "sds.h"

/* The once-per-launch update check.
 *
 * Deliberately NOT a scheduler service: a service re-arms on an interval, and the decision
 * (docs/auto-update.zh.md §0) is one check per launch, not a timer. It is also not on the UI
 * thread and not on the actor runner's loop -- the HTTPS client needs its own round trip and
 * blocking either would freeze the interface for the duration of a network timeout.
 *
 * Shape: a worker thread runs the check, then posts the result back through a uv_async on the
 * caller's loop. That is the same shape the device tools use for their background I/O, so
 * there is one way in this codebase to do "work off-loop, report on-loop".
 */

struct edc_update_service;

/* Called on the loop thread, once, and only when a newer release exists. Never called on a
 * failed check: there is nothing the user could do about a timeout or a rate limit. */
typedef void (*edc_update_found_fn)(const char *tag, const char *asset, void *ud);

/* Start the check.
 *
 * |loop| is the caller's loop -- the result is posted there. |delay_ms| is how long to wait
 * before the request: the launch path is busy with the model request at that moment, and a
 * check that competes with it delays the first thing the user sees.
 *
 * Returns NULL when nothing was started: no check configured, no CA bundle, or the thread
 * could not be created. Every one of those is a "carry on without checking", never an error.
 *
 * |edc_update_service_stop| must be called before the loop is destroyed; it joins the worker
 * and releases everything.
 */
struct edc_update_service *edc_update_service_start(uv_loop_t *loop, const char *cacert_path, const char *api_url,
                                                    const char *current_version, bool allow_pre, uint64_t delay_ms,
                                                    edc_update_found_fn on_found, void *ud);

/* Join the worker and free. Safe to call with NULL, and safe to call from the loop thread
 * (the worker never touches the loop synchronously). */
void edc_update_service_stop(struct edc_update_service *svc);

#endif /* __EDC_UPDATE_SERVICE_H__ */
