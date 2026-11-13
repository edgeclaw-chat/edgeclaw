#include "edc_update_service.h"

#include "edc_update.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

/* One check per launch, off every thread that matters.
 *
 * The worker blocks for up to one timeout, so it cannot be the UI thread and cannot be the
 * actor runner's loop -- either would freeze the interface for the length of a network
 * timeout. It gets its own thread (the HTTPS client runs its own loop inside
 * edc_update_check), and the result is posted back with uv_async. That is the same shape the
 * device tools use for background I/O.
 *
 * The delay before the request is load-bearing rather than cosmetic: at launch the model
 * request is in flight, and a check competing with it delays the first thing the user sees.
 * A semaphore makes the worker wait for the timer instead of merely postponing the report.
 */

struct edc_update_service
{
    uv_thread_t worker;
    bool worker_started;
    uv_sem_t go; /* posted by the delay timer (or by stop) to release the worker */
    bool sem_ready;

    uv_async_t found; /* on the caller's loop */
    uv_timer_t delay; /* on the caller's loop */
    bool found_ready; /* the handle was initialised */
    bool delay_ready;
    bool handles_closing;
    int closes_needed; /* how many close callbacks to wait for before freeing */
    int closes_done;

    sds cacert;
    sds api_url;
    sds current_version;
    bool allow_pre;

    /* Worker writes, loop thread reads in update_on_async. uv_async is the barrier between
     * them, and the values are read exactly once. */
    sds found_tag;
    sds found_asset;
    bool has_result;

    edc_update_found_fn on_found;
    void *ud;
};

static void update_worker(void *arg)
{
    struct edc_update_service *svc = arg;

    uv_sem_wait(&svc->go);

    sds tag = NULL, asset = NULL;
    edc_update_diag diag;
    diag.outcome = EDC_UPDATE_UP_TO_DATE;
    diag.http_status = 0;
    const bool found = edc_update_check_ex(svc->cacert, svc->api_url, svc->current_version, svc->allow_pre,
                                           1u << 20 /* body cap */, 10000 /* ms */, &tag, &asset, &diag);
    if (found)
    {
        svc->found_tag = tag;
        svc->found_asset = asset;
        svc->has_result = true;
        uv_async_send(&svc->found);
        return;
    }

    /* Silent to the user by design: a timeout, a rate limit and a malformed body are not
     * events the user can act on, and the next launch tries again. The REASON is still worth
     * logging, because "nothing to offer" was the line that made a 403 indistinguishable from
     * an up-to-date install while debugging exactly this -- silent is not the same as
     * unrecorded. `edgeclaw --update-check` prints the same distinction for a human. */
    switch (diag.outcome)
    {
    case EDC_UPDATE_UP_TO_DATE:
        log_debug("update check: up to date");
        break;
    case EDC_UPDATE_HTTP_STATUS:
        log_debug("update check: not determined, HTTP %d%s", diag.http_status,
                  diag.http_status == 403 ? " (unauthenticated rate limit)" : "");
        break;
    case EDC_UPDATE_TIMEOUT:
        log_debug("update check: not determined, timed out");
        break;
    case EDC_UPDATE_CONNECT_FAILED:
        log_debug("update check: not determined, could not connect");
        break;
    case EDC_UPDATE_BODY_TOO_LARGE:
        log_debug("update check: not determined, response larger than the cap");
        break;
    case EDC_UPDATE_MALFORMED:
        log_debug("update check: not determined, response is not the release list");
        break;
    default:
        log_debug("update check: not determined");
        break;
    }
    if (tag)
        sdsfree(tag);
    if (asset)
        sdsfree(asset);
}

static void update_on_async(uv_async_t *handle)
{
    struct edc_update_service *svc = handle->data;
    if (!svc || !svc->has_result)
        return;
    svc->has_result = false;
    if (svc->on_found)
        svc->on_found(svc->found_tag, svc->found_asset, svc->ud);
}

static void update_on_delay(uv_timer_t *handle)
{
    struct edc_update_service *svc = handle->data;
    if (!svc)
        return;
    uv_timer_stop(handle);
    uv_sem_post(&svc->go);
}

/* The loop has finished closing a handle. The service is freed only when BOTH closes have
 * come back: uv_close is asynchronous, so freeing the struct from stop() left libuv reading
 * a freed handle in uv__run_closing_handles -- which is exactly the heap-use-after-free ASan
 * reported. */
static void update_close_cb(uv_handle_t *handle)
{
    struct edc_update_service *svc = handle->data;
    if (!svc)
        return;

    /* Both handles share the struct; the second close to arrive does the teardown. */
    if (++svc->closes_done < svc->closes_needed)
        return;

    sdsfree(svc->cacert);
    sdsfree(svc->api_url);
    sdsfree(svc->current_version);
    sdsfree(svc->found_tag);
    sdsfree(svc->found_asset);
    if (svc->sem_ready)
        uv_sem_destroy(&svc->go);
    free(svc);
}

void edc_update_service_stop(struct edc_update_service *svc)
{
    if (!svc)
        return;

    /* Release a worker parked on the semaphore before joining, or the join waits for a delay
     * that will never fire. Posting twice is harmless: the worker waits once. */
    if (svc->sem_ready)
        uv_sem_post(&svc->go);

    if (svc->worker_started)
        uv_thread_join(&svc->worker);

    /* Close, do not destroy: these are loop handles and libuv owns them until the loop has
     * drained. Guarded by the initialised flags, because stop() is also the failure path out
     * of start() -- closing a handle that was never initialised is undefined, and
     * uv_close on a half-built struct is how this would crash rather than fail. */
    if (!svc->handles_closing)
    {
        svc->handles_closing = true;
        int closing = 0;
        if (svc->delay_ready && !uv_is_closing((uv_handle_t *)&svc->delay))
        {
            svc->delay.data = svc; /* the callback reads it back */
            uv_close((uv_handle_t *)&svc->delay, update_close_cb);
            closing++;
        }
        if (svc->found_ready && !uv_is_closing((uv_handle_t *)&svc->found))
        {
            svc->found.data = svc;
            uv_close((uv_handle_t *)&svc->found, update_close_cb);
            closing++;
        }
        if (closing > 0)
        {
            /* The closes own the struct from here: freeing it now is the bug ASan caught, so
             * the last close callback does it (see update_close_cb). */
            svc->closes_needed = closing;
            return;
        }
    }

    /* Nothing was armed (a failure path out of start(), or a second call): no handle will
     * call back, so release here. */
    sdsfree(svc->cacert);
    sdsfree(svc->api_url);
    sdsfree(svc->current_version);
    sdsfree(svc->found_tag);
    sdsfree(svc->found_asset);
    if (svc->sem_ready)
        uv_sem_destroy(&svc->go);
    free(svc);
}

struct edc_update_service *edc_update_service_start(uv_loop_t *loop, const char *cacert_path, const char *api_url,
                                                    const char *current_version, bool allow_pre, uint64_t delay_ms,
                                                    edc_update_found_fn on_found, void *ud)
{
    if (!loop || !api_url || !api_url[0] || !current_version || !on_found)
        return NULL;

    struct edc_update_service *svc = calloc(1, sizeof(*svc));
    if (!svc)
        return NULL;

    svc->cacert = cacert_path ? sdsnew(cacert_path) : NULL;
    svc->api_url = sdsnew(api_url);
    svc->current_version = sdsnew(current_version);
    svc->allow_pre = allow_pre;
    svc->on_found = on_found;
    svc->ud = ud;

    if (!svc->api_url || !svc->current_version)
    {
        edc_update_service_stop(svc);
        return NULL;
    }

    if (uv_sem_init(&svc->go, 0) != 0)
    {
        /* Nothing to release: no worker, nothing armed. Free the strings and go. */
        sdsfree(svc->cacert);
        sdsfree(svc->api_url);
        sdsfree(svc->current_version);
        free(svc);
        return NULL;
    }
    svc->sem_ready = true;

    /* An update check that cannot be armed is a check that does not happen -- not an error at
     * startup. Each of these paths logs at debug and returns NULL. */
    if (uv_async_init(loop, &svc->found, update_on_async) != 0)
    {
        edc_update_service_stop(svc);
        return NULL;
    }
    svc->found_ready = true;
    svc->found.data = svc;

    if (uv_timer_init(loop, &svc->delay) != 0)
    {
        edc_update_service_stop(svc);
        return NULL;
    }
    svc->delay_ready = true;
    svc->delay.data = svc;

    if (uv_thread_create(&svc->worker, update_worker, svc) != 0)
    {
        log_debug("update check: cannot start the worker thread");
        edc_update_service_stop(svc);
        return NULL;
    }
    svc->worker_started = true;

    /* Armed from the loop thread, which is the thread that owns the timer. */
    uv_timer_start(&svc->delay, update_on_delay, delay_ms, 0);
    return svc;
}
