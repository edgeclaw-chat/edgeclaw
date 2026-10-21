/* The sandbox master switch must be OFF wherever there is no backend.
 *
 * Windows has no filesystem-sandbox backend (see edc_platform_has_sandbox in
 * edc_compat.h): no kernel side, and the path semantics the policy layer needs are not
 * implemented there. So the switch must never read "on" on such a platform -- a
 * config.json carried over from Linux/macOS, or one edited by hand, says nothing about
 * what THIS build can enforce.
 *
 * Three places are supposed to keep that promise; this test pins all three:
 *
 *   1. edc_init_env()   writes the truthful default (not a hardcoded true);
 *   2. edc_config_load() clamps whatever is on disk;
 *   3. edc_config_set_sandbox_enabled() refuses to turn it on and does not persist a
 *      "true" that the next load would clamp away.
 *
 * Both directions are asserted -- a platform WITH a backend must keep the switch on --
 * so the test runs everywhere instead of being skipped on the only platform that
 * matters for it. Deliberately free of /tmp and mkdtemp, which is why it can run on
 * Windows at all (compare tests/test_config_sandbox_defaults.c, which cannot). */

#include "edc_compat.h"
#include "edc_platform.h"
#include "edc_config.h"
#include "edc_file_utils.h"
#include "edc_init_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            passed++;                                                                                                  \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* A scratch edc_home under the platform's temp root. mkdtemp() is POSIX-only, so the
 * name is "edc_switch_<pid>": a re-run reuses the same directory and two runs in
 * parallel do not collide. */
static bool scratch_home(char *buf, size_t cap)
{
    const char *base = getenv("TMPDIR");
    if (!base || !base[0])
        base = getenv("TEMP");
    if (!base || !base[0])
        base = getenv("TMP");
    if (!base || !base[0])
        base = "/tmp";

    const int n = snprintf(buf, cap, "%s%cedc_switch_%ld", base, PATH_SEP, (long)getpid());
    return n > 0 && (size_t)n < cap;
}

int main(void)
{
    /* api_key encryption refuses to run without a key when there is no terminal. */
    setenv("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef", 1);

    const bool backend = edc_platform_has_sandbox();

    char home[512];
    if (!scratch_home(home, sizeof(home)))
    {
        fprintf(stderr, "cannot build a scratch path\n");
        return 1;
    }
    EdcError *error = NULL;
    if (!edc_dir_create_with_parents(home, 0755, &error))
    {
        fprintf(stderr, "cannot create %s: %s\n", home, error ? error->message : "?");
        if (error)
            edc_error_free(error);
        return 1;
    }

    /* The scratch name is per pid and a pid can be reused, so a config.json left by an
     * earlier run would silently become this run's fixture (it did: an old one carried the
     * POSIX whitelist). Start from a known state. */
    {
        char stale[600];
        snprintf(stale, sizeof(stale), "%s%cconfig.json", home, PATH_SEP);
        remove(stale);
    }

    /* ── 1. a fresh config records what this build can actually do ─────────── */
    CHECK(edc_init_env(home, true), "edc_init_env creates config.json");

    struct edc_config *cfg = edc_config_load(home);
    CHECK(cfg != NULL, "the fresh config loads");
    if (!cfg)
        return 1;
    CHECK(cfg->enable_sandbox == backend,
          backend ? "fresh config: sandbox on (this platform has a backend)"
                  : "fresh config: sandbox OFF (no backend on this platform)");

    /* The default path LISTS are platform facts as well: where nothing is enforced there is
     * nothing to whitelist, so the POSIX whitelist must not be written into a file that
     * then claims /usr is readable (on Windows that resolves to C:\usr). */
    {
        const bool have_ro = cfg->sandbox_ro_paths && cfg->sandbox_ro_paths[0] != NULL;
        CHECK(have_ro == backend, backend ? "ro defaults exist where there is a backend"
                                          : "no ro defaults where there is no backend");
    }
    edc_config_free(cfg);

    /* ── the file itself: one spelling, no foreign paths ───────────────────── */
    {
        char path[600];
        char text[8192];
        snprintf(path, sizeof(path), "%s%cconfig.json", home, PATH_SEP);

        FILE *f = fopen(path, "rb");
        CHECK(f != NULL, "config.json exists");
        if (f)
        {
            const size_t n = fread(text, 1, sizeof(text) - 1, f);
            text[n] = '\0';
            fclose(f);

            if (!backend)
            {
                CHECK(strstr(text, "/proc") == NULL,
                      "the POSIX whitelist is not written where there is no backend");
                CHECK(strstr(text, "/tmp") == NULL, "/tmp is not a writable path where there is no backend");
            }
#ifdef _WIN32
            /* The entry used to be built with "%s/workspace", so the file carried
             * "C:\Users\x\.edgeclaw/workspace" -- one spelling per directory, which is what
             * the file is for. */
            CHECK(strstr(text, "/workspace") == NULL, "the workspace entry is not a mixed-separator spelling");
            CHECK(strstr(text, "\\\\workspace") != NULL,
                  "the workspace entry is written with the platform separator");
#endif
        }
    }

    /* ── 2. a config that claims "on" is clamped where there is no backend ──
     * edc_config_update_sandbox_enabled() is the raw writer (it does not consult the
     * platform), so this reproduces both the hand-edited file and the one copied from a
     * machine that does have a backend. */
    CHECK(edc_config_update_sandbox_enabled(home, true), "config.json can say enable=true");

    cfg = edc_config_load(home);
    CHECK(cfg != NULL, "the edited config loads");
    if (!cfg)
        return 1;
    CHECK(cfg->enable_sandbox == backend,
          backend ? "enable=true is honoured where there is a backend"
                  : "enable=true on disk is clamped to false (no backend)");

    /* ── 3. the runtime switch refuses to turn it on where there is no backend ── */
    const bool was = cfg->enable_sandbox;
    const bool ok = edc_config_set_sandbox_enabled(cfg, true);
    if (backend)
    {
        CHECK(ok, "enabling succeeds where there is a backend");
        CHECK(cfg->enable_sandbox, "the live config is updated");
    }
    else
    {
        CHECK(!ok, "enabling is refused where there is no backend");
        CHECK(cfg->enable_sandbox == was, "the live config is left untouched");
        CHECK(cfg->enable_sandbox == false, "the switch stays off");
    }
    edc_config_free(cfg);

    /* Nothing was persisted behind our back: a reload agrees with what we just saw. */
    cfg = edc_config_load(home);
    CHECK(cfg != NULL, "the config reloads after the refusal");
    CHECK(cfg && cfg->enable_sandbox == backend, "the persisted state still matches the platform");
    edc_config_free(cfg);

    printf("%s: %d passed, %d failed (backend=%d)\n", backend ? "with backend" : "no backend", passed, failed,
           (int)backend);
    return failed == 0 ? 0 : 1;
}
