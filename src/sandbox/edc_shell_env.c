#include "edc_shell_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The allowlist. Names that are not set in the parent are simply skipped, so
 * the Windows-required variables below are harmless on POSIX.
 */
static const char *const shell_env_allowlist[] = {
    /* POSIX */
    "PATH",
    "HOME",
    "USER",
    "LOGNAME",
    "SHELL",
    "LANG",
    "LC_ALL",
    "LC_CTYPE",
    "TERM",
    "TZ",
    "TMPDIR",
#ifdef _WIN32
    /* Windows: cmd.exe and friends fail without these */
    "SystemRoot",
    "windir",
    "ComSpec",
    "PATHEXT",
    "TEMP",
    "TMP",
    "USERPROFILE",
    "HOMEDRIVE",
    "HOMEPATH",
    "USERNAME",
    "NUMBER_OF_PROCESSORS",
    "PROCESSOR_ARCHITECTURE",
    "OS",
#endif
    NULL,
};

bool edc_shell_env_name_allowed(const char *name)
{
    if (!name)
        return false;
    for (size_t i = 0; shell_env_allowlist[i]; i++)
        if (strcmp(shell_env_allowlist[i], name) == 0)
            return true;
    return false;
}

char **edc_shell_env_build(void)
{
    size_t cap = sizeof(shell_env_allowlist) / sizeof(shell_env_allowlist[0]);
    char **envp = calloc(cap, sizeof(*envp));
    if (!envp)
        return NULL;

    size_t n = 0;
    for (size_t i = 0; shell_env_allowlist[i]; i++)
    {
        const char *name = shell_env_allowlist[i];
        const char *val = getenv(name);
        if (!val)
            continue;
        size_t len = strlen(name) + 1 + strlen(val) + 1;
        char *entry = malloc(len);
        if (!entry)
            break;
        snprintf(entry, len, "%s=%s", name, val);
        envp[n++] = entry;
    }
    envp[n] = NULL;
    return envp;
}

void edc_shell_env_free(char **envp)
{
    if (!envp)
        return;
    for (size_t i = 0; envp[i]; i++)
        free(envp[i]);
    free(envp);
}
