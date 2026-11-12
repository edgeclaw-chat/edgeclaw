#include "edc_update.h"

#include "cJSON.h"
#include "edc_https_client.h"
#include "edc_semver.h"

#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* The public release list. Not /releases/latest: that endpoint skips pre-releases, and every
 * tag this project has published carries a suffix. */
#define EDC_UPDATE_RELEASES_URL "https://api.github.com/repos/edgeclaw-chat/edgeclaw/releases"

/* Default delay before the launch-time request; see edc_update_check_delay_ms(). */
#define EDC_UPDATE_CHECK_DELAY_MS 10000

/* The asset target for this build. Mirrors tools/install.sh's platform table -- those two
 * must agree, because one downloads what the other would have.
 *
 * No libc-musl distinction: the musl builds are published under their own target names, but a
 * running binary knows which one it is only by its own name, and getting that wrong would
 * fetch a build that cannot exec. The glibc target is the safe default; the pre-replace
 * `--version` probe refuses a binary that will not run. */
const char *edc_update_target(void)
{
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "edgeclaw_aarch64_windows";
#else
    return "edgeclaw_amd64_windows";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "edgeclaw_aarch64_macos";
#else
    return "edgeclaw_amd64_macos";
#endif
#else
#if defined(__aarch64__)
    return "edgeclaw_aarch64_linux";
#elif defined(__arm__)
    return "edgeclaw_arm_linux";
#else
    return "edgeclaw_amd64_linux";
#endif
#endif
}

/* The release endpoint, honouring EDGECLAW_UPDATE_RELEASES_URL. See edc_update.h for why. */
sds edc_update_releases_url(void)
{
    const char *env = getenv("EDGECLAW_UPDATE_RELEASES_URL");
    return sdsnew((env && *env) ? env : EDC_UPDATE_RELEASES_URL);
}

/* The version to compare against, honouring EDGECLAW_UPDATE_CURRENT_VERSION. */
const char *edc_update_current_version(const char *fallback)
{
    const char *env = getenv("EDGECLAW_UPDATE_CURRENT_VERSION");
    return (env && *env) ? env : fallback;
}

/* Delay before the launch-time request, honouring EDGECLAW_UPDATE_CHECK_DELAY_MS. */
uint64_t edc_update_check_delay_ms(void)
{
    const char *env = getenv("EDGECLAW_UPDATE_CHECK_DELAY_MS");
    if (env && *env)
    {
        const long v = strtol(env, NULL, 10);
        if (v >= 0)
            return (uint64_t)v;
    }
    return EDC_UPDATE_CHECK_DELAY_MS;
}

/* The archive name for |tag| on this target; the release workflow builds exactly this. */
static sds asset_name(const char *tag)
{
    return sdscatprintf(sdsempty(), "%s_%s.tar.gz", edc_update_target(), tag);
}

/* Does this release carry the archive we need? A release can exist without an asset for this
 * platform (a partial matrix), and offering it would produce a 404 at download time. */
static bool release_has_asset(cJSON *assets, const char *want)
{
    if (!assets || !cJSON_IsArray(assets))
        return false;
    for (int i = 0; i < cJSON_GetArraySize(assets); i++)
    {
        cJSON *a = cJSON_GetArrayItem(assets, i);
        cJSON *name = a ? cJSON_GetObjectItem(a, "name") : NULL;
        if (name && cJSON_IsString(name) && strcmp(name->valuestring, want) == 0)
            return true;
    }
    return false;
}

/* ── the network half ───────────────────────────────────────────────────────
 * Everything above is pure; this is the one part that talks to the world. It is kept in its
 * own translation unit's worth of code (but the same file, so the selection rules and the
 * fetch stay in step) because the tests drive it through a local HTTP server instead of
 * GitHub.
 *
 * The blocking shape is copied deliberately from edc_endpoint_probe: the HTTPS client runs on
 * its own uv loop, so the caller waits on a condvar with a deadline. Sharing that shape means
 * one fewer thing that behaves differently from the rest of the codebase.
 */

struct update_state
{
    uv_mutex_t m;
    uv_cond_t c;
    int done;
    int status;  /* the status line, 0 when none arrived */
    int err;     /* an ERROR event fired (DNS/TLS/timeout) */
    sds body;    /* accumulated response body */
    size_t max;  /* stop accumulating past this */
    int over;    /* the body exceeded |max| */
};

static void update_on_event(struct edc_https_request *req, int ev, void *data)
{
    (void)data;
    struct update_state *st = edc_https_request_get_priv(req);
    if (!st)
        return;

    switch (ev)
    {
    case EDC_HTTPS_EVENT_HS: {
        /* No Authorization header: the releases endpoint is public, and sending a token to
         * it would be both unnecessary and a leak. */
        const char *path = edc_https_request_get_path(req);
        const char *host = edc_https_request_get_host(req);
        sds hdr = sdscatprintf(sdsempty(),
                               "GET %s HTTP/1.1\r\nHost: %s\r\n"
                               "User-Agent: edgeclaw-update-check\r\n"
                               "Accept: application/vnd.github+json\r\n"
                               "Connection: close\r\n\r\n",
                               path && path[0] ? path : "/", host ? host : "");
        edc_https_request_write(req, (const unsigned char *)hdr, (size_t)sdslen(hdr));
        sdsfree(hdr);
        break;
    }
    case EDC_HTTPS_EVENT_HEADER:
        st->status = edc_https_request_get_status(req);
        break;
    case EDC_HTTPS_EVENT_MSG: {
        /* data is a uv_buf_t* owned by the caller -- it must be freed here either way. */
        uv_buf_t *buf = data;
        if (buf && buf->base)
        {
            if (!st->over && st->body && sdslen(st->body) + buf->len > st->max)
            {
                /* Stop keeping bytes, but let the transfer finish so the socket closes
                 * normally rather than through a teardown race. */
                st->over = 1;
            }
            if (!st->over && st->body)
                st->body = sdscatlen(st->body, buf->base, buf->len);
            free(buf->base);
        }
        break;
    }
    case EDC_HTTPS_EVENT_ERROR:
        st->err = 1;
        break;
    case EDC_HTTPS_EVENT_CLOSE:
        uv_mutex_lock(&st->m);
        st->done = 1;
        uv_cond_signal(&st->c);
        uv_mutex_unlock(&st->m);
        break;
    default:
        break;
    }
}

bool edc_update_check(const char *cacert_path, const char *api_url, const char *current_version, bool allow_pre,
                      size_t max_bytes, int timeout_ms, sds *out_tag, sds *out_asset)
{
    if (out_tag)
        *out_tag = NULL;
    if (out_asset)
        *out_asset = NULL;
    if (!api_url || !api_url[0] || !current_version)
        return false;

    struct edc_https_client *client = edc_https_client_new(cacert_path);
    if (!client)
        return false;

    struct update_state st;
    memset(&st, 0, sizeof(st));
    uv_mutex_init(&st.m);
    uv_cond_init(&st.c);
    st.body = sdsempty();
    st.max = max_bytes > 0 ? max_bytes : 1u << 20; /* 1 MiB: the release list is far smaller */

    bool ok = false;
    if (edc_https_client_connect(client, api_url, update_on_event, &st) == 0)
    {
        uv_mutex_lock(&st.m);
        while (!st.done)
        {
            if (uv_cond_timedwait(&st.c, &st.m, (uint64_t)(timeout_ms > 0 ? timeout_ms : 1) * 1000000ULL) != 0)
                break; /* deadline: treat exactly like a failed check */
        }
        const int done = st.done, status = st.status, err = st.err, over = st.over;
        uv_mutex_unlock(&st.m);

        /* 200 only. A 403 here is the unauthenticated rate limit (60/hour/IP) and is expected
         * on a shared address -- silent, like every other failure. */
        if (done && !err && !over && status == 200)
            ok = edc_update_pick(st.body, current_version, allow_pre, out_tag, out_asset);
    }

    sdsfree(st.body);
    uv_cond_destroy(&st.c);
    uv_mutex_destroy(&st.m);
    edc_https_client_free(client);
    return ok;
}

bool edc_update_pick(const char *releases_json, const char *current_version, bool allow_pre, sds *out_tag,
                     sds *out_asset)
{
    if (out_tag)
        *out_tag = NULL;
    if (out_asset)
        *out_asset = NULL;
    if (!releases_json || !current_version)
        return false;

    cJSON *root = cJSON_Parse(releases_json);
    if (!root)
        return false;
    if (!cJSON_IsArray(root))
    {
        cJSON_Delete(root);
        return false;
    }

    const sds target = sdsnew(edc_update_target());
    sds best_tag = NULL;
    sds best_asset = NULL;

    for (int i = 0; i < cJSON_GetArraySize(root); i++)
    {
        cJSON *rel = cJSON_GetArrayItem(root, i);
        if (!rel || !cJSON_IsObject(rel))
            continue;

        /* Drafts are invisible on the Releases page; a release candidate is only offered when
         * the caller asked for pre-releases. GitHub's own `prerelease` flag is the authority
         * here -- deriving it from the tag suffix would disagree with the release page. */
        cJSON *draft = cJSON_GetObjectItem(rel, "draft");
        if (draft && cJSON_IsTrue(draft))
            continue;
        cJSON *pre = cJSON_GetObjectItem(rel, "prerelease");
        if (!allow_pre && pre && cJSON_IsTrue(pre))
            continue;

        cJSON *tag = cJSON_GetObjectItem(rel, "tag_name");
        if (!tag || !cJSON_IsString(tag) || !tag->valuestring[0])
            continue;

        /* Only releases that actually carry this platform's archive are candidates, so an
         * older platform build is offered rather than a newer release that lacks it. */
        sds want = asset_name(tag->valuestring);
        const bool has = release_has_asset(cJSON_GetObjectItem(rel, "assets"), want);
        sdsfree(want);
        if (!has)
            continue;

        /* Strictly newer than the running build; equal is "already current". */
        if (!edc_semver_is_newer(tag->valuestring, current_version))
            continue;

        /* Keep the highest: the API returns newest-first, but relying on that would make the
         * result depend on a server-side ordering nobody promises. */
        if (best_tag && !edc_semver_is_newer(tag->valuestring, best_tag))
            continue;

        sdsfree(best_tag);
        sdsfree(best_asset);
        best_tag = sdsnew(tag->valuestring);
        best_asset = asset_name(tag->valuestring);
    }

    sdsfree(target);
    cJSON_Delete(root);

    if (!best_tag)
        return false;

    if (out_tag)
        *out_tag = best_tag;
    else
        sdsfree(best_tag);
    if (out_asset)
        *out_asset = best_asset;
    else
        sdsfree(best_asset);
    return true;
}
