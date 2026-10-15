#include "edc_endpoint_probe.h"

#include "edc_https_client.h"

#include "sds.h"
#include <stdio.h>
#include <string.h>
#include <uv.h>

/* A tiny chat request: enough for the endpoint to answer (200) or say why not (401 /
 * 400 / 404), the same shape the python generator probes with. */
#define EDC_PROBE_BODY "{\"model\":\"probe\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],\"max_tokens\":1}"

struct probe_state
{
    uv_mutex_t m;
    uv_cond_t c;
    int done;
    int status; /* the response status line, 0 when none arrived */
    int err;    /* an ERROR event fired (TLS/DNS/... failure) */
    sds api_key;
};

/* Runs on the client's own thread (see edc_https_client.c). */
static void probe_on_event(struct edc_https_request *req, int ev, void *data)
{
    (void)data;
    struct probe_state *st = edc_https_request_get_priv(req);
    if (!st)
        return;
    switch (ev)
    {
    case EDC_HTTPS_EVENT_HS: {
        const char *path = edc_https_request_get_path(req);
        const char *host = edc_https_request_get_host(req);
        sds hdr =
            sdscatprintf(sdsempty(),
                         "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                         "Authorization: Bearer %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                         path ? path : "/", host ? host : "", st->api_key ? st->api_key : "", strlen(EDC_PROBE_BODY));
        edc_https_request_write(req, (const unsigned char *)hdr, (size_t)sdslen(hdr));
        sdsfree(hdr);
        edc_https_request_write(req, (const unsigned char *)EDC_PROBE_BODY, strlen(EDC_PROBE_BODY));
        break;
    }
    case EDC_HTTPS_EVENT_HEADER:
        st->status = edc_https_request_get_status(req);
        break;
    case EDC_HTTPS_EVENT_ERROR:
        st->err = 1;
        break;
    case EDC_HTTPS_EVENT_CLOSE:
        /* Signal under the lock: the waiter checks + waits on the same mutex, so no
         * wakeup is lost. */
        uv_mutex_lock(&st->m);
        st->done = 1;
        uv_cond_signal(&st->c);
        uv_mutex_unlock(&st->m);
        break;
    default:
        break;
    }
}

bool edc_endpoint_probe(const char *cacert_path, const char *api_url, const char *api_key, int timeout_ms, char *out,
                        size_t cap)
{
    if (out && cap)
        out[0] = 0;
    if (!api_url || !api_url[0])
    {
        snprintf(out, cap, "no base URL");
        return false;
    }

    struct edc_https_client *client = edc_https_client_new(cacert_path);
    if (!client)
    {
        snprintf(out, cap, "cannot start the HTTPS client");
        return false;
    }

    struct probe_state st;
    memset(&st, 0, sizeof(st));
    uv_mutex_init(&st.m);
    uv_cond_init(&st.c);
    st.api_key = sdsnew(api_key ? api_key : "");

    bool answered = false;
    if (edc_https_client_connect(client, api_url, probe_on_event, &st) != 0)
    {
        snprintf(out, cap, "bad URL");
    }
    else
    {
        uv_mutex_lock(&st.m);
        while (!st.done)
        {
            if (uv_cond_timedwait(&st.c, &st.m, (uint64_t)(timeout_ms > 0 ? timeout_ms : 1) * 1000000ULL) != 0)
                break; /* timed out */
        }
        int status = st.status, err = st.err, done = st.done;
        uv_mutex_unlock(&st.m);

        if (done && status > 0)
        {
            answered = true;
            snprintf(out, cap, "reached — HTTP %d%s", status, status == 200 ? " (OK)" : " (check key / model)");
        }
        else if (err)
        {
            snprintf(out, cap, "connection failed");
        }
        else
        {
            snprintf(out, cap, "no response (timed out)");
        }
    }

    sdsfree(st.api_key);
    uv_mutex_destroy(&st.m);
    uv_cond_destroy(&st.c);
    edc_https_client_free(client); /* joins the client thread */
    return answered;
}
