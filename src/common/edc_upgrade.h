#ifndef __EDC_UPGRADE_H__
#define __EDC_UPGRADE_H__

#include <stdbool.h>

#include "sds.h"

/* Upgrading, by handing the job to the installer that put the binary there.
 *
 * This module decides WHETHER a self-service upgrade is possible and WHAT to run. It performs
 * no downloads and writes no files: the installer already knows how to fetch, verify a
 * signature, verify a checksum and replace the binary atomically, and duplicating that here
 * would mean two implementations of one security check.
 *
 * Split so the interesting parts are testable without a subprocess:
 *
 *   edc_upgrade_detect()    path -> method        (pure)
 *   edc_upgrade_argv()      method+version -> argv (pure)
 *   edc_upgrade_run()       the only part that spawns anything
 */

/* How this binary got here -- which determines whether we may upgrade it at all. */
typedef enum
{
    /* Unknown: the binary is somewhere an installer would not have put it. An upgrade is
     * reported as unavailable with manual instructions, never attempted. Guessing wrong here
     * means running an installer against a directory nobody meant it to touch. */
    EDC_UPGRADE_METHOD_UNKNOWN = 0,

    /* Installed by tools/install.sh, which defaults to $HOME/.local/bin. */
    EDC_UPGRADE_METHOD_INSTALLER,
} edc_upgrade_method;

const char *edc_upgrade_method_name(edc_upgrade_method m);

/* Absolute path of the running executable, or NULL when it cannot be determined.
 * Caller frees. */
sds edc_platform_exe_path(void);

/* Map an executable's path to the method that installed it. Pure: no filesystem access, so
 * the rules can be pinned by a table. |home| is passed explicitly rather than read from the
 * environment for the same reason. */
edc_upgrade_method edc_upgrade_detect(const char *exe_path, const char *home);

/* The command that performs the upgrade, as an argv vector.
 *
 * |script_path| is a local copy of the installer for |tag| (see §8.1 of the design: the
 * script is fetched pinned to that tag so it and the archive come from the same release).
 * |dir| is the directory the running binary lives in. Both are passed explicitly: the
 * installer must not re-decide the version (the UI already told the user which one) and must
 * not re-decide the location (it must land where the running one is).
 *
 * Returns a NULL-terminated array of owned strings, or NULL when the method has no command
 * (UNKNOWN, or a platform without a usable shell). Caller frees with
 * edc_upgrade_argv_free(). */
sds *edc_upgrade_argv(edc_upgrade_method method, const char *script_path, const char *tag, const char *dir);

void edc_upgrade_argv_free(sds *argv);

/* Why no command could be produced, for the message shown to the user. Never NULL. */
const char *edc_upgrade_unavailable_reason(edc_upgrade_method method);

/* The command a user would type by hand, for the paths that refuse to act (an unknown
 * method, or an installer that would not start). Windows prints a PowerShell form: its
 * installer exists, so the hint is a shortcut there rather than a last resort. Caller
 * frees. */
sds edc_upgrade_manual_hint(const char *tag);

/* Whether this build can offer an in-app upgrade at all.
 *
 * Always true for a shipped platform: what differs is which installer runs, not whether one
 * exists (see edc_upgrade_installer_filename). Kept as a function rather than a constant so
 * a future platform without a native installer has one place to say so. */
bool edc_upgrade_supported(void);

/* The installer this platform runs: "install.sh" on POSIX, "install.ps1" on Windows.
 *
 * The caller fetches it from `<tag>/tools/<filename>` and passes the local copy to
 * edc_upgrade_argv(). Windows needs its own because a default install has no `sh` -- the
 * Windows quick start is "unpack the tarball with the bundled tar and run the exe", so
 * requiring Git for Windows to enable updates would be a new dependency, not a convenience. */
const char *edc_upgrade_installer_filename(void);

/* Stage a copy of |body| in a private temporary file and return its path, or NULL.
 *
 * A staging step rather than an argument passed straight to the shell, because the script
 * must be the exact bytes the release published -- a shell reading it from stdin would share
 * a stream with whatever else the child reads.
 *
 * Two things differ per platform, and only two: where a temporary file belongs (TMPDIR or
 * /tmp; the variable GetTempPathA reads) and the name's extension. `powershell -File`
 * REFUSES a file whose name does not end in `.ps1` ("the file does not have a '.ps1'
 * extension"), so the name is load-bearing there; POSIX runs it as `sh <path>`, where an
 * extension would only mislead -- that script is install.sh.
 *
 * Everything else is one body. It was two, and the Windows half returned NULL on the grounds
 * that "there is no shell to run the script anyway" -- true until the PowerShell installer
 * existed, and it stayed behind afterwards, so every Windows upgrade failed at this step.
 * Sharing the body is also what makes the failure paths measurable: the coverage job runs on
 * Linux only, so code behind `#ifdef _WIN32` is code nothing reports on, and the difference
 * between "measured low" and "not measurable" is easy to lose. */
sds edc_upgrade_stage_script(const char *body);

/* Remove a staged installer. Tolerates NULL and a path that is already gone. */
void edc_upgrade_discard_script(const char *path);

/* The environment handed to the installer: the parent's, minus this program's own variables.
 *
 * Inherit, then subtract -- rather than an allowlist -- because the installer needs a working
 * machine environment and every hand-picked list has been too small. It started as proxy
 * variables only, which on Windows dropped PROCESSOR_ARCHITECTURE (the installer refuses to
 * pick a target without it: "Fail : PROCESSOR_ARCHITECTURE is not set"), then PATHEXT (so
 * `tar` resolved to nothing and extraction failed with "The term 'tar' is not recognized"),
 * then TEMP. Each was a separate bug report. A child process that inherits what a user's
 * shell would have is the definition of working here.
 *
 * What is subtracted is everything whose name starts with `EDGECLAW_`: the master key, the
 * catalog override, the HTTPS timeout, the theme. The installer has no use for any of them,
 * and the design says so explicitly (docs/auto-update.zh.md section 8.2: inherit, but drop
 * EDGECLAW_MASTER_KEY). Matching the whole prefix rather than that one name is deliberate: a
 * variable added later is private by default instead of leaking until someone notices.
 *
 * Returns a NULL-terminated array of owned "NAME=value" strings (freed with
 * edc_upgrade_env_free), or NULL when nothing could be collected. */
sds *edc_upgrade_child_env(void);

/* Free an array from edc_upgrade_child_env. Tolerates NULL. */
void edc_upgrade_env_free(sds *env);

/* Whether |entry| ("NAME=value") is one this program keeps out of the installer's
 * environment. Exposed so the rule can be tested on every platform rather than only where a
 * spawn happens. */
bool edc_upgrade_env_is_private(const char *entry);

#endif /* __EDC_UPGRADE_H__ */
