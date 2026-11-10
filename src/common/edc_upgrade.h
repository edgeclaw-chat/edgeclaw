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

/* The command a user would type by hand, for the paths that refuse to act (unknown method,
 * no shell, Windows). Caller frees. */
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

#endif /* __EDC_UPGRADE_H__ */
