/* Redirect handling in edc_https_client.
 *
 * The client follows 301/302/303/307/308 so that a GitHub release download works: the
 * `github.com/.../releases/download/<tag>/<asset>` URL answers 302 to
 * `objects.githubusercontent.com`, and without following it a downloader gets a redirect page
 * instead of an archive. That gap is why this exists.
 *
 * Everything here is offline: the two decisions that matter -- "is this status a redirect"
 * and "what does this Location mean" -- are pure functions, so they are tested directly
 * rather than through a local TLS server or a live request.
 *
 * The properties pinned below are the ones whose absence would be silent:
 *   - a downgrade to plaintext is refused, not followed;
 *   - 304 is not treated as a redirect (it validates the request already made);
 *   - a relative Location resolves against the right base;
 *   - an unparseable/empty Location is refused rather than followed as a guess.
 */

#include "edc_https_client_internal.h"
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

/* Compare a resolved URL with the expectation, freeing the result. */
static void expect(const char *base, const char *location, const char *want, const char *label)
{
    sds got = edc_https_resolve_redirect(base, location);
    const bool ok = want ? (got && strcmp(got, want) == 0) : (got == NULL);
    if (!ok)
        fprintf(stderr, "  %s: got %s, want %s\n", label, got ? got : "(null)", want ? want : "(null)");
    CHECK(ok, label);
    if (got)
        sdsfree(got);
}

/* ── the follow/fail decision, exhaustively ──────────────────────────────── */
/* Every combination of (is-redirect, has-Location, budget) is enumerated, because each one
 * has a distinct consequence and three of them are easy to get subtly wrong:
 *
 *   - a redirect WITH budget but WITHOUT Location would, if "followed", re-request the same
 *     URL forever;
 *   - a redirect with NO budget must fail, not silently deliver a redirect page as content;
 *   - a non-redirect must ignore the budget entirely.
 */
static void test_redirect_decide(void)
{
    printf("\n-- the follow/fail decision --\n");

    /* Non-redirect statuses: never a redirect, whatever else is true. */
    const int non_redirect[] = {200, 201, 204, 304, 400, 404, 500, 0};
    for (size_t i = 0; i < sizeof(non_redirect) / sizeof(non_redirect[0]); i++)
    {
        for (int has_loc = 0; has_loc <= 1; has_loc++)
        {
            for (int budget = 0; budget <= 1; budget++)
            {
                char label[128];
                snprintf(label, sizeof(label), "status %d (loc=%d, budget=%d) is delivered as-is", non_redirect[i],
                         has_loc, budget);
                CHECK(edc_https_redirect_decide(non_redirect[i], has_loc != 0, budget) == EDC_HTTPS_REDIRECT_NONE,
                      label);
            }
        }
    }

    /* Redirect statuses, with a Location and budget: followed. */
    const int redirects[] = {301, 302, 303, 307, 308};
    for (size_t i = 0; i < sizeof(redirects) / sizeof(redirects[0]); i++)
    {
        char label[128];
        snprintf(label, sizeof(label), "status %d with a Location and budget is followed", redirects[i]);
        CHECK(edc_https_redirect_decide(redirects[i], true, 1) == EDC_HTTPS_REDIRECT_FOLLOW, label);
        snprintf(label, sizeof(label), "status %d with a Location and budget 5 is followed", redirects[i]);
        CHECK(edc_https_redirect_decide(redirects[i], true, 5) == EDC_HTTPS_REDIRECT_FOLLOW, label);

        /* Budget exhausted: fail rather than deliver the redirect page as the resource. */
        snprintf(label, sizeof(label), "status %d with a Location and NO budget fails", redirects[i]);
        CHECK(edc_https_redirect_decide(redirects[i], true, 0) == EDC_HTTPS_REDIRECT_FAIL, label);
        snprintf(label, sizeof(label), "status %d with a Location and negative budget fails", redirects[i]);
        CHECK(edc_https_redirect_decide(redirects[i], true, -1) == EDC_HTTPS_REDIRECT_FAIL, label);

        /* No Location: cannot be followed, so it is delivered and the caller sees the 3xx.
         * Following it would re-request the same URL with no progress. */
        snprintf(label, sizeof(label), "status %d without a Location is delivered, not followed", redirects[i]);
        CHECK(edc_https_redirect_decide(redirects[i], false, 5) == EDC_HTTPS_REDIRECT_NONE, label);
        snprintf(label, sizeof(label), "status %d without a Location ignores the budget", redirects[i]);
        CHECK(edc_https_redirect_decide(redirects[i], false, 0) == EDC_HTTPS_REDIRECT_NONE, label);
    }
}

int main(void)
{
    /* ── which statuses are redirects ─────────────────────────────────────── */
    CHECK(edc_https_status_is_redirect(301), "301 is a redirect");
    CHECK(edc_https_status_is_redirect(302), "302 is a redirect");
    CHECK(edc_https_status_is_redirect(303), "303 is a redirect");
    CHECK(edc_https_status_is_redirect(307), "307 is a redirect");
    CHECK(edc_https_status_is_redirect(308), "308 is a redirect");

    /* 304 means "your cached copy is current", which is an answer, not a new location.
     * Following it would turn a conditional request into a request for a URL that does not
     * exist. */
    CHECK(!edc_https_status_is_redirect(304), "304 is NOT a redirect");
    CHECK(!edc_https_status_is_redirect(200), "200 is not a redirect");
    CHECK(!edc_https_status_is_redirect(404), "404 is not a redirect");
    CHECK(!edc_https_status_is_redirect(0), "no status is not a redirect");

    /* ── the form GitHub actually uses ────────────────────────────────────── */
    expect("https://github.com/edgeclaw-chat/edgeclaw/releases/download/v1.0.0/a.tar.gz",
           "https://objects.githubusercontent.com/github-production-release-asset-2e65be/1/2?X-Amz-Signature=abc",
           "https://objects.githubusercontent.com/github-production-release-asset-2e65be/1/2?X-Amz-Signature=abc",
           "an absolute https Location is taken as-is");

    /* ── downgrade refusal ────────────────────────────────────────────────── */
    /* The archive is signature-checked, so a plaintext hop would not by itself let a false
     * archive through -- but the request would be readable on the wire for no reason, and
     * the client is shared with callers that send more than a URL. */
    expect("https://example.com/a", "http://example.com/b", NULL, "a downgrade to http is refused");
    expect("https://example.com/a", "HTTP://example.com/b", NULL, "the downgrade check is case-insensitive");

    /* ── relative forms ──────────────────────────────────────────────────── */
    expect("https://example.com/dir/file", "/other/asset", "https://example.com/other/asset",
           "an absolute path keeps the origin");
    expect("https://example.com/dir/file", "//cdn.example.com/x", "https://cdn.example.com/x",
           "a protocol-relative Location becomes https");
    expect("https://example.com/dir/file", "sibling", "https://example.com/dir/sibling",
           "a relative path resolves against the base directory");
    expect("https://example.com/dir/file", "./up", "https://example.com/dir/./up",
           "a dot-relative path is joined (normalisation is the server's job)");

    /* A non-default port has to survive, or a test server on :8443 would be redirected to a
     * real host on :443. */
    expect("https://example.com:8443/dir/file", "/x", "https://example.com:8443/x",
           "a non-default port is preserved");
    expect("https://example.com:443/dir/file", "/x", "https://example.com/x",
           "the default port is not spelled out");

    /* ── refusals ────────────────────────────────────────────────────────── */
    expect("https://example.com/a", "", NULL, "an empty Location is refused");
    expect("https://example.com/a", NULL, NULL, "a missing Location is refused");

    test_redirect_decide();

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
