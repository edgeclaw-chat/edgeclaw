#include "edc_update.h"

#include "cJSON.h"
#include "edc_semver.h"

#include <string.h>

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
