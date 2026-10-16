/* First-run wizard config: the persisted `onboarded` flag and the context-window writer.
 *
 * The write path is exercised for real: edc_init_env(home, true) creates config.json,
 * the writers mutate it, and edc_config_load(home) reads it back the way startup does. */

#include "edc_config.h"
#include "edc_init_env.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0;
static int failed = 0;

#define T_CHECK(cond, msg)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            passed++;                                                                                                  \
        else                                                                                                           \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool file_has_key(const char *home, const char *key)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/config.json", home);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    return strstr(buf, pat) != NULL;
}

int main(void)
{
    /* The secret module (api_key encryption) refuses to run without a key when there is
     * no terminal; the same variable the other config tests set. */
    setenv("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef", 1);

    char home[] = "/tmp/edc_onboarded_XXXXXX";
    if (!mkdtemp(home))
    {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    T_CHECK(edc_init_env(home, true), "edc_init_env creates config.json");

    /* A fresh config is NOT onboarded. */
    struct edc_config *cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "config loads");
    T_CHECK(cfg && !cfg->onboarded, "fresh config => onboarded = false");
    edc_config_free(cfg);

    /* set_onboarded persists AND updates the live config. */
    cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "config reloads");
    T_CHECK(cfg && edc_config_set_onboarded(cfg, true), "set_onboarded(true) succeeds");
    T_CHECK(cfg && cfg->onboarded, "the live config was updated");
    edc_config_free(cfg);
    T_CHECK(file_has_key(home, "onboarded"), "the key was written to config.json");
    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->onboarded, "reload => onboarded = true");
    edc_config_free(cfg);

    /* context_window writer: positive only, preserving other keys. */
    T_CHECK(!edc_config_update_context_window(home, 0), "context_window <= 0 is refused");
    T_CHECK(edc_config_update_context_window(home, 12345), "context_window(12345) succeeds");
    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->context_window == 12345, "reload => context_window = 12345");
    T_CHECK(cfg && cfg->llm_api_conf.api_url && cfg->llm_api_conf.api_url[0],
            "other keys survive the edit (api_url still set)");
    edc_config_free(cfg);

    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
