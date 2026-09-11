#include "edc_shell_env.h"
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

/* True when envp has an entry literally starting with "name=". */
static bool env_has(char **envp, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; envp[i]; i++)
        if (strncmp(envp[i], name, n) == 0 && envp[i][n] == '=')
            return true;
    return false;
}

/* True when any entry carries a token-looking value. */
static bool env_looks_like_secret(char **envp)
{
    for (size_t i = 0; envp[i]; i++)
        if (strstr(envp[i], "sk-") || strstr(envp[i], "TOKEN"))
            return true;
    return false;
}

int main(void)
{
    /* 1. The allowlist predicate. */
    T_CHECK(edc_shell_env_name_allowed("PATH"), "PATH is allowed");
    T_CHECK(edc_shell_env_name_allowed("HOME"), "HOME is allowed");
    T_CHECK(edc_shell_env_name_allowed("LANG"), "LANG is allowed");
    T_CHECK(edc_shell_env_name_allowed("TZ"), "TZ is allowed");
    T_CHECK(!edc_shell_env_name_allowed("LLM_API_TOKEN"), "LLM_API_TOKEN is not allowed");
    T_CHECK(!edc_shell_env_name_allowed("LLM_API_URL"), "LLM_API_URL is not allowed");
    T_CHECK(!edc_shell_env_name_allowed("SSH_CONNECTION"), "SSH_CONNECTION is not allowed");
    T_CHECK(!edc_shell_env_name_allowed("LD_PRELOAD"), "LD_PRELOAD is not allowed");
    T_CHECK(!edc_shell_env_name_allowed("EDGECLAW_MASTER_KEY"), "EDGECLAW_MASTER_KEY is not allowed");
    T_CHECK(!edc_shell_env_name_allowed(NULL), "NULL is not allowed");

    /* 2. build() forwards exactly the allowlisted, present variables -- and
     *    drops the secrets that used to leak through uv_spawn(env=NULL). */
    setenv("PATH", "/usr/bin:/bin", 1);
    setenv("HOME", "/home/testuser", 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("LLM_API_TOKEN", "sk-deadbeefdeadbeef", 1);
    setenv("SSH_CONNECTION", "1.2.3.4 1 5.6.7.8 2", 1);
    setenv("LD_PRELOAD", "/tmp/evil.so", 1);

    char **envp = edc_shell_env_build();
    T_CHECK(envp != NULL, "build() returns non-NULL");
    T_CHECK(env_has(envp, "PATH"), "PATH is forwarded");
    T_CHECK(env_has(envp, "HOME"), "HOME is forwarded");
    T_CHECK(env_has(envp, "LANG"), "LANG is forwarded");
    T_CHECK(!env_has(envp, "LLM_API_TOKEN"), "LLM_API_TOKEN is dropped");
    T_CHECK(!env_has(envp, "SSH_CONNECTION"), "SSH_CONNECTION is dropped");
    T_CHECK(!env_has(envp, "LD_PRELOAD"), "LD_PRELOAD is dropped");
    T_CHECK(!env_looks_like_secret(envp), "no token-looking entry leaked");

    /* Every forwarded name must itself be allowlisted. */
    bool all_allowed = true;
    for (size_t i = 0; envp[i]; i++)
    {
        char *eq = strchr(envp[i], '=');
        if (!eq)
        {
            all_allowed = false;
            continue;
        }
        *eq = '\0';
        if (!edc_shell_env_name_allowed(envp[i]))
            all_allowed = false;
        *eq = '=';
    }
    T_CHECK(all_allowed, "every forwarded entry is allowlisted");
    edc_shell_env_free(envp);

    /* 3. free() is NULL-safe. */
    edc_shell_env_free(NULL);

    printf("test_shell_env: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
