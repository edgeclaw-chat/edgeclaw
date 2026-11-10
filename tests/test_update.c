/* Release selection for the updater.
 *
 * This is the decision "should the user be told about a newer version, and which file would
 * they get". It is pure -- the JSON GitHub returns goes in, a tag and an asset name come out --
 * so the rules can be pinned without a network or a fake HTTP server.
 *
 * Every failure here has the same safe answer: say nothing. An updater that reports a
 * problem is an updater that gets turned off.
 */

#include "edc_update.h"
#include "sds.h"

#include <stdio.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            g_passed++;                                                                                                \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            g_failed++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* Run the picker and check the outcome. |want_tag| NULL means "nothing should be offered". */
static void expect(const char *json, const char *current, bool allow_pre, const char *want_tag, const char *label)
{
    sds tag = NULL, asset = NULL;
    const bool got = edc_update_pick(json, current, allow_pre, &tag, &asset);
    if (want_tag == NULL)
    {
        if (got)
            fprintf(stderr, "  %s: offered %s, expected nothing\n", label, tag ? tag : "(null)");
        CHECK(!got, label);
    }
    else if (!got)
    {
        fprintf(stderr, "  %s: offered nothing, expected %s\n", label, want_tag);
        CHECK(false, label);
    }
    else
    {
        if (!tag || strcmp(tag, want_tag) != 0)
            fprintf(stderr, "  %s: tag %s, want %s\n", label, tag ? tag : "(null)", want_tag);
        CHECK(tag && strcmp(tag, want_tag) == 0, label);
        /* The asset must be this platform's, and named after the tag it came from. */
        CHECK(asset && strstr(asset, edc_update_target()) != NULL, "the asset belongs to this target");
        CHECK(asset && strstr(asset, want_tag) != NULL, "the asset carries the chosen tag");
        CHECK(asset && strstr(asset, ".tar.gz") != NULL, "the asset is the archive");
    }
    if (tag)
        sdsfree(tag);
    if (asset)
        sdsfree(asset);
}

/* A release object carrying this platform's asset, with the asset named after |tag| -- the
 * workflow builds `<target>_<tag>.tar.gz`, so a fixture whose asset name disagrees with its
 * tag is not a real release shape (and the picker correctly skips it). */
static const char *RELEASE_TPL = "{\"tag_name\":\"%s\",\"draft\":%s,\"prerelease\":%s,\"assets\":[%s]}";

/* "<target>_<tag>.tar.gz" as the assets array entry for |tag|. Written into |buf|. */
static const char *asset_for(char *buf, size_t cap, const char *tag)
{
    snprintf(buf, cap, "{\"name\":\"%s_%s.tar.gz\"}", edc_update_target(), tag);
    return buf;
}

int main(void)
{
    char buf[8192];
    char asset[512];
    const char *target = edc_update_target();

    /* ── an asset entry for this platform ────────────────────────────────── */
    snprintf(asset, sizeof(asset), "{\"name\":\"%s_v1.0.1.tar.gz\"}", target);

    /* ── the ordinary case ────────────────────────────────────────────────── */
    {
        char one[1024];
        snprintf(one, sizeof(one), RELEASE_TPL, "v1.0.1", "false", "false", asset);
        snprintf(buf, sizeof(buf), "[%s]", one);
        expect(buf, "1.0.0", false, "v1.0.1", "a newer stable release with this platform's asset is offered");
        /* The same release against its own version is not an update. */
        expect(buf, "1.0.1", false, NULL, "the running version is not offered as an update");
        /* Nor against something newer. */
        expect(buf, "1.1.0", false, NULL, "an older release is not offered");
    }

    /* ── pre-release policy ───────────────────────────────────────────────── */
    {
        char one[1024];
        char rc_asset[512];
        snprintf(one, sizeof(one), RELEASE_TPL, "v1.0.1-rc.1", "false", "true",
                 asset_for(rc_asset, sizeof(rc_asset), "v1.0.1-rc.1"));
        snprintf(buf, sizeof(buf), "[%s]", one);
        /* A stable build must not be pushed onto a release candidate. */
        expect(buf, "1.0.0", false, NULL, "a pre-release is not offered to a stable channel");
        expect(buf, "1.0.0", true, "v1.0.1-rc.1", "a pre-release IS offered on the pre channel");
        /* ... and a pre-release of the version already running is not an update either. */
        expect(buf, "1.0.1-rc.1", true, NULL, "an equal pre-release is not offered");
    }

    /* ── drafts are invisible ─────────────────────────────────────────────── */
    {
        char one[1024];
        snprintf(one, sizeof(one), RELEASE_TPL, "v9.9.9", "true", "false", asset);
        snprintf(buf, sizeof(buf), "[%s]", one);
        expect(buf, "1.0.0", false, NULL, "a draft release is never offered");
    }

    /* ── a release without this platform's asset ─────────────────────────── */
    /* This is the partial-matrix case: offering it would produce a 404 at download time. */
    {
        char one[1024];
        snprintf(one, sizeof(one), RELEASE_TPL, "v1.0.1", "false", "false", "");
        snprintf(buf, sizeof(buf), "[%s]", one);
        expect(buf, "1.0.0", false, NULL, "a release with no asset for this platform is skipped");
    }

    /* ── several releases: the highest wins, regardless of order ──────────── */
    {
        char a[1024], b[1024], c[1024];
        char aa[512], ba[512], ca[512];
        snprintf(a, sizeof(a), RELEASE_TPL, "v1.0.1", "false", "false", asset_for(aa, sizeof(aa), "v1.0.1"));
        snprintf(b, sizeof(b), RELEASE_TPL, "v1.2.0", "false", "false", asset_for(ba, sizeof(ba), "v1.2.0"));
        snprintf(c, sizeof(c), RELEASE_TPL, "v1.1.0", "false", "false", asset_for(ca, sizeof(ca), "v1.1.0"));
        /* Newest-first, as GitHub returns them. */
        snprintf(buf, sizeof(buf), "[%s,%s,%s]", b, c, a);
        expect(buf, "1.0.0", false, "v1.2.0", "the highest version wins (newest-first input)");
        /* And again with the list scrambled: the answer must not depend on server ordering. */
        snprintf(buf, sizeof(buf), "[%s,%s,%s]", a, c, b);
        expect(buf, "1.0.0", false, "v1.2.0", "the highest version wins (scrambled input)");
        /* A pre-release must not outrank a release of the same number in the same list. */
        char rca[512];
        snprintf(c, sizeof(c), RELEASE_TPL, "v1.2.0-rc.1", "false", "true",
                 asset_for(rca, sizeof(rca), "v1.2.0-rc.1"));
        snprintf(buf, sizeof(buf), "[%s,%s]", c, b);
        expect(buf, "1.0.0", true, "v1.2.0", "a release outranks its own pre-release");
    }

    /* ── an unparseable current version offers nothing ───────────────────── */
    /* This is the `dev` build: no tag, nothing to compare, and it must never try to replace
     * itself with a release. */
    {
        char one[1024];
        snprintf(one, sizeof(one), RELEASE_TPL, "v1.0.1", "false", "false", asset);
        snprintf(buf, sizeof(buf), "[%s]", one);
        expect(buf, "dev", false, NULL, "an unparseable current version offers nothing");
        expect(buf, "", false, NULL, "an empty current version offers nothing");
    }

    /* ── malformed input is silent ────────────────────────────────────────── */
    expect("not json", "1.0.0", false, NULL, "malformed JSON offers nothing");
    expect("{}", "1.0.0", false, NULL, "a JSON object instead of an array offers nothing");
    expect("[]", "1.0.0", false, NULL, "an empty release list offers nothing");
    expect(NULL, "1.0.0", false, NULL, "a missing body offers nothing");

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
