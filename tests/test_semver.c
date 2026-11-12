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

/* Validity is a separate question from precedence: is_newer() answers false for both "not
 * newer" and "cannot parse", and the updater has to tell those apart. */
static void check_valid(const char *v, bool want, const char *label)
{
    const bool got = edc_semver_is_valid(v);
    if (got != want)
    {
        fprintf(stderr, "FAIL %s: is_valid(%s) = %d, want %d\n", label, v ? v : "(null)", got, want);
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

    /* ── numeric edges ───────────────────────────────────────────────────── */
    /* Component values are compared as numbers, so a lexical compare would make 1.10.0 look
     * older than 1.9.0 -- and then nobody using 1.9.0 would ever be told about 1.10.0. */
    check("1.10.0", "1.9.0", true, "1.10.0 is newer than 1.9.0 (numeric, not lexical)");
    check("1.0.10", "1.0.9", true, "1.0.10 is newer than 1.0.9");
    check("1.9.0", "1.10.0", false, "1.9.0 is not newer than 1.10.0");

    /* A component too long to be a real version is refused rather than silently wrapped by
     * the accumulator -- a wrapped value could make an absurd version look newer. Nine
     * digits is the ceiling; 999999999 is a legitimate (if silly) version. */
    check("1.0.1000000000", "1.0.0", false, "a 10-digit patch is refused");
    check("9999999990.0.0", "1.0.0", false, "a 10-digit major is refused");
    check("999999999.0.0", "1.0.0", true, "nine digits is still a valid version");

    /* Pre-release numeric identifiers may be long but still compare by value. */
    check("1.0.0-rc.0000000001", "1.0.0-rc.1", false, "leading zeros do not make an identifier bigger");
    check("1.0.0-rc.2", "1.0.0-rc.0000000001", true, "rc.2 is newer than rc.0000000001");

    /* ── a pre-release with build metadata ───────────────────────────────── */
    check("1.0.0-rc.1+build.7", "1.0.0-rc.1", false, "metadata on a pre-release does not change precedence");
    check("1.0.0-rc.2+build.7", "1.0.0-rc.1", true, "the pre-release part still counts with metadata");

    /* A bare "v" and a lone major are not versions. */
    check("v", "1.0.0", false, "a bare v is refused");
    check("v1", "1.0.0", false, "a lone major is refused");
    check("1.0.0", "v", false, "a bare v as the current version is refused");

    /* ── is this a version at all ─────────────────────────────────────────── */
    /* Separate from "is it newer": that one answers false both for "not newer" and for "cannot
     * parse", and the updater has to tell them apart -- an unparseable version means the whole
     * feature is off, which is worth saying out loud. */
    check_valid("1.0.0", true, "a release version is valid");
    check_valid("v1.0.0", true, "the tag form is valid");
    check_valid("1.0.0-rc.1", true, "a pre-release is valid");
    check_valid("1.0.0+build.5", true, "build metadata is valid");
    check_valid("89e3d2c", false, "a git commit hash is not a version");
    check_valid("89e3d2c-dirty", false, "a dirty commit hash is not a version");
    check_valid("dev", false, "the dev placeholder is not a version");
    check_valid("", false, "empty is not a version");
    check_valid(NULL, false, "NULL is not a version");

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
