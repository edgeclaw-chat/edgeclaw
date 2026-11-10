/* Semantic-version precedence, for "is there a newer release than this build".
 *
 * The cases below are the ones an updater gets wrong. Every one of them decides whether a
 * user sees an update offer, so a mistake is not a crash -- it is either a user stuck on an
 * old build or one nagged forever about the version they already have.
 *
 * Precedence rules are semver 2.0.0 §11; the interesting half is pre-releases, because this
 * project tags them (v1.0.0-rc.1) and they must NOT be offered to a stable build.
 */

#include "edc_semver.h"

#include <stdio.h>

static int g_passed = 0;
static int g_failed = 0;

static void check(const char *cand, const char *cur, bool want, const char *label)
{
    const bool got = edc_semver_is_newer(cand, cur);
    if (got != want)
    {
        fprintf(stderr, "FAIL %s: %s vs %s: got %s, want %s\n", label, cand, cur, got ? "newer" : "not-newer",
                want ? "newer" : "not-newer");
        g_failed++;
        return;
    }
    g_passed++;
}

int main(void)
{
    /* ── ordinary ordering ────────────────────────────────────────────────── */
    check("1.0.1", "1.0.0", true, "a patch bump is newer");
    check("1.1.0", "1.0.9", true, "a minor bump outranks a patch bump");
    check("2.0.0", "1.99.99", true, "a major bump outranks everything");
    check("1.0.0", "1.0.1", false, "the same version is not newer");
    check("1.0.0", "1.0.0", false, "an identical version is not newer");
    check("1.0.0", "1.1.0", false, "an older version is not newer");

    /* The tag form is what GitHub reports, so it has to compare against a build that knows
     * its own version without the prefix. */
    check("v1.2.0", "1.1.0", true, "a leading v is accepted on the candidate");
    check("1.2.0", "v1.1.0", true, "a leading v is accepted on the current version");
    check("v1.0.0", "v1.0.0", false, "the same tag is not newer");

    /* ── pre-releases sort before their release ───────────────────────────── */
    /* This is the pair that matters for this project: if it were reversed, a 1.0.0 build
     * would be offered its own release candidate. */
    check("1.0.0-rc.1", "1.0.0", false, "a pre-release is NOT newer than its release");
    check("1.0.0", "1.0.0-rc.1", true, "the release IS newer than its pre-release");
    check("1.0.1-rc.1", "1.0.0", true, "a pre-release of a higher version is still newer");
    check("1.0.0-rc.2", "1.0.0-rc.1", true, "rc.2 is newer than rc.1");

    /* Numeric identifiers compare by value: a string compare would put rc.10 before rc.2 and
     * strand users on rc.2 forever. */
    check("1.0.0-rc.10", "1.0.0-rc.2", true, "rc.10 is newer than rc.2 (numeric, not lexical)");
    check("1.0.0-rc.2", "1.0.0-rc.10", false, "rc.2 is not newer than rc.10");

    /* More fields win when the shared prefix is equal. */
    check("1.0.0-rc.1.1", "1.0.0-rc.1", true, "rc.1.1 is newer than rc.1");
    check("1.0.0-rc.1", "1.0.0-rc.1.1", false, "rc.1 is not newer than rc.1.1");

    /* Alphanumeric identifiers compare bytewise, and a numeric one is lower than an
     * alphanumeric one. */
    check("1.0.0-beta", "1.0.0-alpha", true, "beta is newer than alpha");
    check("1.0.0-alpha.1", "1.0.0-alpha", true, "alpha.1 is newer than alpha");

    /* ── build metadata does not participate ──────────────────────────────── */
    check("1.0.0+build.5", "1.0.0", false, "build metadata alone is not newer");
    check("1.0.1+build.5", "1.0.0", true, "the version part still counts with metadata");
    check("1.0.0", "1.0.0+build.5", false, "metadata on the current version changes nothing");

    /* ── unparseable input must never produce an offer ────────────────────── */
    /* A build with no git tag reports "dev"; if that compared as newer, every such build
     * would be told to update itself. */
    check("1.0.0", "dev", false, "an unparseable current version is not newer");
    check("dev", "1.0.0", false, "an unparseable candidate is not newer");
    check("1.0", "1.0.0", false, "a two-part version is refused");
    check("1.0.0.0", "1.0.0", false, "a four-part version is refused");
    check("1.0.0-", "1.0.0", false, "an empty pre-release suffix is refused");
    check("", "1.0.0", false, "an empty candidate is refused");
    check("1.0.0", "", false, "an empty current version is refused");
    check("1.0.0-dirty", "1.0.0", false, "a dirty suffix is a pre-release, not newer than the release");

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
