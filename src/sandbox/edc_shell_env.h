#ifndef EDC_SHELL_ENV_H
#define EDC_SHELL_ENV_H

#include <stdbool.h>

/*
 * Minimal environment whitelist for commands spawned by the shell tool.
 *
 * libuv's uv_spawn() inherits the *entire* parent environment when
 * options.env == NULL. The app is normally started from the user's login
 * shell, so that environment carries things a command has no business seeing
 * -- API tokens (e.g. an LLM_API_TOKEN exported from ~/.zshrc), SSH_* session
 * info, DBUS/XDG handles, cloud credentials, ... -- which would leak into
 * every `sh -c` child. Only a small allowlist of variables a command needs to
 * locate binaries, its home, its locale and its temp dir is forwarded.
 */

/* True when |name| is in the forwarded allowlist. */
bool edc_shell_env_name_allowed(const char *name);

/* Build a NULL-terminated envp of "NAME=value" entries for the allowlisted
 * variables that are set in the current environment. Returns NULL only on
 * allocation failure (the caller may then choose to inherit the environment).
 * Never returns the app's full environment. Free with edc_shell_env_free(). */
char **edc_shell_env_build(void);

/* Free an envp returned by edc_shell_env_build(). NULL-safe. */
void edc_shell_env_free(char **envp);

#endif /* EDC_SHELL_ENV_H */
