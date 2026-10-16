/* The shell-like syntax switch: default OFF, and it round-trips through config.json.
 *
 * The write path is exercised for real: edc_init_env(home, true) creates config.json,
 * edc_config_set_shell_syntax mutates it, and edc_config_load reads it back the way
 * startup does. (Same shape as test_config_onboarded.) */

#include "edc_config.h"
#include "edc_init_env.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
    /* The secret module (api_key encryption) refuses to run without a key when there is
     * no terminal; the same variable the other config tests set. */
    setenv("EDGECLAW_MASTER_KEY", "0123456789abcdef0123456789abcdef", 1);

    char home[] = "/tmp/edc_shell_syntax_XXXXXX";
    if (!mkdtemp(home))
    {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    T_CHECK(edc_init_env(home, true), "edc_init_env creates config.json");

    /* A fresh config has the switch OFF: it is an opt-in, because the operators spell out
     * a Unix shell vocabulary and a user who does not expect them gets a silently
     * half-parsed task rather than an error. */
    struct edc_config *cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "config loads");
    T_CHECK(cfg && !cfg->shell_syntax, "fresh config => shell_syntax = false (the default)");
    edc_config_free(cfg);

    /* set_shell_syntax persists AND updates the live config. */
    cfg = edc_config_load(home);
    T_CHECK(cfg != NULL, "config reloads");
    T_CHECK(cfg && edc_config_set_shell_syntax(cfg, true), "set_shell_syntax(true) succeeds");
    T_CHECK(cfg && cfg->shell_syntax, "the live config was updated");
    T_CHECK(cfg && cfg->llm_api_conf.api_url && cfg->llm_api_conf.api_url[0] != '\0',
            "other keys survive the edit (api_url still set)");
    edc_config_free(cfg);

    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->shell_syntax, "reload => shell_syntax = true");
    edc_config_free(cfg);

    /* ... and back off, which is what `/shell off` does. */
    cfg = edc_config_load(home);
    T_CHECK(cfg && edc_config_set_shell_syntax(cfg, false), "set_shell_syntax(false) succeeds");
    edc_config_free(cfg);

    cfg = edc_config_load(home);
    T_CHECK(cfg && !cfg->shell_syntax, "reload => false again");
    edc_config_free(cfg);

    /* The bare updater (what the setter calls) works too, and a NULL home is refused
     * rather than crashing. */
    T_CHECK(!edc_config_update_shell_syntax(NULL, true), "a NULL edc_home is refused");
    T_CHECK(edc_config_update_shell_syntax(home, true), "update_shell_syntax(home, true) succeeds");

    cfg = edc_config_load(home);
    T_CHECK(cfg && cfg->shell_syntax, "reload => true");
    edc_config_free(cfg);

    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
