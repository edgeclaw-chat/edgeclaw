/* Deciding whether an upgrade can be delegated, and what to run.
 *
 * Everything here is offline and pure: a path maps to a method, and (method, path, tag, dir)
 * maps to an argv. Those are the decisions that can be silently wrong -- an over-eager match
 * runs an installer against a directory nobody meant it to touch -- so they are pinned by
 * table rather than checked by hand.
 *
 * The rules exist because the installer is the only thing that knows how to fetch, verify a
 * signature and replace the binary atomically. This module's whole job is to hand it the work
 * and to say "no" when that is not safe.
 */

#include "edc_upgrade.h"
#include "sds.h"

#include <stdbool.h>
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

static void expect_method(const char *exe, const char *home, edc_upgrade_method want, const char *label)
{
    const edc_upgrade_method got = edc_upgrade_detect(exe, home);
    if (got != want)
        fprintf(stderr, "  %s: got %s, want %s\n", label, edc_upgrade_method_name(got), edc_upgrade_method_name(want));
    CHECK(got == want, label);
}

/* Compare an argv with the expected words. |want| is NULL-terminated. */
static void expect_argv(edc_upgrade_method m, const char *script, const char *tag, const char *dir,
                        const char *const *want, const char *label)
{
    sds *got = edc_upgrade_argv(m, script, tag, dir);
    if (!want)
    {
        CHECK(got == NULL, label);
        if (got)
            edc_upgrade_argv_free(got);
        return;
    }
    if (!got)
    {
        fprintf(stderr, "  %s: got NULL argv\n", label);
        CHECK(false, label);
        return;
    }
    int i = 0;
    bool ok = true;
    for (; want[i]; i++)
    {
        if (!got[i] || strcmp(got[i], want[i]) != 0)
        {
            fprintf(stderr, "  %s: argv[%d] = %s, want %s\n", label, i, got[i] ? got[i] : "(null)", want[i]);
            ok = false;
            break;
        }
    }
    if (ok && got[i] != NULL)
    {
        fprintf(stderr, "  %s: argv has extra entries after %d\n", label, i);
        ok = false;
    }
    CHECK(ok, label);
    edc_upgrade_argv_free(got);
}

int main(void)
{
    /* ── the executable's directory decides everything ────────────────────── */
    expect_method("/home/u/.local/bin/edgeclaw", "/home/u", EDC_UPGRADE_METHOD_INSTALLER,
                  "the installer's default target is recognised");
    expect_method("/home/u/bin/edgeclaw", "/home/u", EDC_UPGRADE_METHOD_INSTALLER,
                  "the conventional per-user bin is recognised");

    /* A trailing separator must not change the answer: edc_path_dirname may hand one back,
     * and a path spelled with one names the same directory. */
    expect_method("/home/u/.local/bin/edgeclaw", "/home/u/", EDC_UPGRADE_METHOD_INSTALLER,
                  "a trailing separator on home changes nothing");

    /* ── and everything else must NOT be recognised ───────────────────────────
     * This is the half that matters. Matching too eagerly means running an installer
     * against a directory that nobody put the binary in. */
    expect_method("/usr/local/bin/edgeclaw", "/home/u", EDC_UPGRADE_METHOD_UNKNOWN, "a system-wide install is unknown");
    expect_method("/opt/edgeclaw/edgeclaw", "/home/u", EDC_UPGRADE_METHOD_UNKNOWN, "/opt is unknown");
    expect_method("/home/u/workspace/github/edgeclaw/bazel-bin/src/tui/edgeclaw", "/home/u",
                  EDC_UPGRADE_METHOD_UNKNOWN, "a build tree is unknown");
    expect_method("/home/u/.local/bin-backup/edgeclaw", "/home/u", EDC_UPGRADE_METHOD_UNKNOWN,
                  "a sibling directory that merely STARTS with the same name is unknown");
    expect_method("/tmp/home/u/.local/bin/edgeclaw", "/home/u", EDC_UPGRADE_METHOD_UNKNOWN,
                  "the right suffix inside an unrelated directory is unknown");
    expect_method("/home/other/.local/bin/edgeclaw", "/home/u", EDC_UPGRADE_METHOD_UNKNOWN,
                  "another user's install directory is unknown");
    expect_method("/home/u/.local/bin/nested/edgeclaw", "/home/u", EDC_UPGRADE_METHOD_UNKNOWN,
                  "a subdirectory of the install directory is unknown");

    /* ── degenerate input never claims it can upgrade ─────────────────────── */
    expect_method(NULL, "/home/u", EDC_UPGRADE_METHOD_UNKNOWN, "a missing path is unknown");
    expect_method("", "/home/u", EDC_UPGRADE_METHOD_UNKNOWN, "an empty path is unknown");
    expect_method("/home/u/.local/bin/edgeclaw", NULL, EDC_UPGRADE_METHOD_UNKNOWN, "a missing home is unknown");
    expect_method("/home/u/.local/bin/edgeclaw", "", EDC_UPGRADE_METHOD_UNKNOWN, "an empty home is unknown");
    /* A bare filename has no directory to compare, so it cannot be recognised. */
    expect_method("edgeclaw", "/home/u", EDC_UPGRADE_METHOD_UNKNOWN, "a bare filename is unknown");

    /* ── the command ──────────────────────────────────────────────────────── */
#if !defined(_WIN32)
    {
        static const char *const want[] = {"sh", "/tmp/x/install.sh", "--tag", "v1.2.3", "--dir", "/home/u/.local/bin",
                                           NULL};
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, "/tmp/x/install.sh", "v1.2.3", "/home/u/.local/bin", want,
                    "the installer command carries the script, the tag and the directory");

        /* The tag reaches the installer verbatim -- including a pre-release suffix, which is
         * what this project's tags look like. */
        static const char *const pre[] = {"sh", "/tmp/x/install.sh", "--tag", "v1.0.1-rc.1", "--dir", "/home/u/.local/bin",
                                          NULL};
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, "/tmp/x/install.sh", "v1.0.1-rc.1", "/home/u/.local/bin", pre,
                    "a pre-release tag is passed through unchanged");

        /* A path with a space survives: this is argv, not a shell string, so no quoting is
         * needed and none is added. */
        static const char *const spaced[] = {"sh", "/tmp/a b/install.sh", "--tag", "v1.2.3", "--dir", "/home/u/my bin",
                                             NULL};
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, "/tmp/a b/install.sh", "v1.2.3", "/home/u/my bin", spaced,
                    "a path containing a space stays one argument");

        /* ── nothing is produced when it could not be run ────────────────── */
        expect_argv(EDC_UPGRADE_METHOD_UNKNOWN, "/tmp/x/install.sh", "v1.2.3", "/home/u/.local/bin", NULL,
                    "an unknown method produces no command");
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, NULL, "v1.2.3", "/d", NULL, "a missing script path produces no command");
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, "", "v1.2.3", "/d", NULL, "an empty script path produces no command");
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, "/s", NULL, "/d", NULL, "a missing tag produces no command");
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, "/s", "", "/d", NULL, "an empty tag produces no command");
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, "/s", "v1.2.3", NULL, NULL, "a missing dir produces no command");
        expect_argv(EDC_UPGRADE_METHOD_INSTALLER, "/s", "v1.2.3", "", NULL, "an empty dir produces no command");
    }

    CHECK(edc_upgrade_supported(), "POSIX can upgrade");
#endif

    /* ── the manual fallback always says something usable ─────────────────── */
    {
        sds hint = edc_upgrade_manual_hint("v1.2.3");
        CHECK(hint && strstr(hint, "v1.2.3") != NULL, "the manual hint names the version");
        CHECK(hint && strstr(hint, "install.sh") != NULL, "the manual hint names the installer");
        /* It must be pinned to the tag, not a moving branch -- the same rule the automated
         * path follows. */
        CHECK(hint && strstr(hint, "/v1.2.3/tools/install.sh") != NULL, "the manual hint is pinned to the tag");
        sdsfree(hint);
    }
    {
        /* With no version yet (a failed check), the hint still has to read sensibly. */
        sds hint = edc_upgrade_manual_hint(NULL);
        CHECK(hint && strstr(hint, "<version>") != NULL, "a versionless hint uses a placeholder");
        sdsfree(hint);
    }

    /* ── reasons are never NULL, whatever the method ──────────────────────── */
    CHECK(edc_upgrade_unavailable_reason(EDC_UPGRADE_METHOD_UNKNOWN) != NULL, "unknown has a reason");
    CHECK(edc_upgrade_unavailable_reason(EDC_UPGRADE_METHOD_INSTALLER) != NULL, "installer has a reason");
    CHECK(edc_upgrade_unavailable_reason((edc_upgrade_method)999) != NULL, "an out-of-range method has a reason");

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
